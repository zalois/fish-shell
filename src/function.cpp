// Functions for storing and retrieving function information. These functions also take care of
// autoloading functions in the $fish_function_path. Actual function evaluation is taken care of by
// the parser and to some degree the builtin handling library.
//
#include "config.h"  // IWYU pragma: keep

// IWYU pragma: no_include <type_traits>
#include <dirent.h>
#include <pthread.h>
#include <stddef.h>
#include <wchar.h>

#include <map>
#include <memory>
#include <unordered_set>
#include <string>
#include <unordered_map>
#include <utility>

#include "autoload.h"
#include "common.h"
#include "env.h"
#include "event.h"
#include "fallback.h"  // IWYU pragma: keep
#include "function.h"
#include "intern.h"
#include "parser_keywords.h"
#include "reader.h"
#include "wutil.h"  // IWYU pragma: keep

/// Table containing all functions.
typedef std::unordered_map<wcstring, function_info_t> function_map_t;
static function_map_t loaded_functions;

/// Functions that shouldn't be autoloaded (anymore).
static std::unordered_set<wcstring> function_tombstones;

/// Lock for functions.
static std::recursive_mutex functions_lock;

static bool function_remove_ignore_autoload(const wcstring &name, bool tombstone = true);

/// Callback when an autoloaded function is removed.
void autoloaded_function_removed(const wcstring &cmd) {
    function_remove_ignore_autoload(cmd, false);
}

// Function autoloader
static autoload_t function_autoloader(L"fish_function_path", autoloaded_function_removed);

/// Kludgy flag set by the load function in order to tell function_add that the function being
/// defined is autoloaded. There should be a better way to do this...
static bool is_autoload = false;

/// Make sure that if the specified function is a dynamically loaded function, it has been fully
/// loaded.
static int load(const wcstring &name) {
    ASSERT_IS_MAIN_THREAD();
    scoped_rlock locker(functions_lock);
    bool was_autoload = is_autoload;
    int res;

    bool no_more_autoload = function_tombstones.count(name) > 0;
    if (no_more_autoload) return 0;

    function_map_t::iterator iter = loaded_functions.find(name);
    if (iter != loaded_functions.end() && !iter->second.is_autoload) {
        // We have a non-autoload version already.
        return 0;
    }

    is_autoload = true;
    res = function_autoloader.load(name, true);
    is_autoload = was_autoload;
    return res;
}

/// Insert a list of all dynamically loaded functions into the specified list.
static void autoload_names(std::unordered_set<wcstring> &names, int get_hidden) {
    size_t i;

    const auto path_var = env_get(L"fish_function_path");
    if (path_var.missing_or_empty()) return;

    wcstring_list_t path_list;
    path_var->to_list(path_list);

    for (i = 0; i < path_list.size(); i++) {
        const wcstring &ndir_str = path_list.at(i);
        const wchar_t *ndir = (wchar_t *)ndir_str.c_str();
        DIR *dir = wopendir(ndir);
        if (!dir) continue;

        wcstring name;
        while (wreaddir(dir, name)) {
            const wchar_t *fn = name.c_str();
            const wchar_t *suffix;
            if (!get_hidden && fn[0] == L'_') continue;

            suffix = wcsrchr(fn, L'.');
            if (suffix && (wcscmp(suffix, L".fish") == 0)) {
                wcstring name(fn, suffix - fn);
                names.insert(name);
            }
        }
        closedir(dir);
    }
}

static std::map<wcstring, env_var_t> snapshot_vars(const wcstring_list_t &vars) {
    std::map<wcstring, env_var_t> result;
    for (wcstring_list_t::const_iterator it = vars.begin(), end = vars.end(); it != end; ++it) {
        auto var = env_get(*it);
        if (var) result.insert(std::make_pair(*it, std::move(*var)));
    }
    return result;
}

function_info_t::function_info_t(const function_data_t &data, const wchar_t *filename,
                                 int def_offset, bool autoload)
    : definition(data.definition),
      description(data.description),
      definition_file(intern(filename)),
      definition_offset(def_offset),
      named_arguments(data.named_arguments),
      inherit_vars(snapshot_vars(data.inherit_vars)),
      is_autoload(autoload),
      shadow_scope(data.shadow_scope) {}

function_info_t::function_info_t(const function_info_t &data, const wchar_t *filename,
                                 int def_offset, bool autoload)
    : definition(data.definition),
      description(data.description),
      definition_file(intern(filename)),
      definition_offset(def_offset),
      named_arguments(data.named_arguments),
      inherit_vars(data.inherit_vars),
      is_autoload(autoload),
      shadow_scope(data.shadow_scope) {}

void function_add(const function_data_t &data, const parser_t &parser, int definition_line_offset) {
    UNUSED(parser);
    ASSERT_IS_MAIN_THREAD();

    CHECK(!data.name.empty(), );  //!OCLINT(multiple unary operator)
    CHECK(data.definition, );
    scoped_rlock locker(functions_lock);

    // Remove the old function.
    function_remove(data.name);

    // Create and store a new function.
    const wchar_t *filename = reader_current_filename();

    const function_map_t::value_type new_pair(
        data.name, function_info_t(data, filename, definition_line_offset, is_autoload));
    loaded_functions.insert(new_pair);

    // Add event handlers.
    for (std::vector<event_t>::const_iterator iter = data.events.begin(); iter != data.events.end();
         ++iter) {
        event_add_handler(*iter);
    }
}

int function_exists(const wcstring &cmd) {
    if (parser_keywords_is_reserved(cmd)) return 0;
    scoped_rlock locker(functions_lock);
    load(cmd);
    return loaded_functions.find(cmd) != loaded_functions.end();
}

void function_load(const wcstring &cmd) {
    if (!parser_keywords_is_reserved(cmd)) {
        scoped_rlock locker(functions_lock);
        load(cmd);
    }
}

int function_exists_no_autoload(const wcstring &cmd, const env_vars_snapshot_t &vars) {
    if (parser_keywords_is_reserved(cmd)) return 0;
    scoped_rlock locker(functions_lock);
    return loaded_functions.find(cmd) != loaded_functions.end() ||
           function_autoloader.can_load(cmd, vars);
}

static bool function_remove_ignore_autoload(const wcstring &name, bool tombstone) {
    // Note: the lock may be held at this point, but is recursive.
    scoped_rlock locker(functions_lock);

    function_map_t::iterator iter = loaded_functions.find(name);

    // Not found.  Not erasing.
    if (iter == loaded_functions.end()) return false;

    // Removing an auto-loaded function.  Prevent it from being auto-reloaded.
    if (iter->second.is_autoload && tombstone) function_tombstones.insert(name);

    loaded_functions.erase(iter);
    event_t ev(EVENT_ANY);
    ev.function_name = name;
    event_remove(ev);
    return true;
}

void function_remove(const wcstring &name) {
    if (function_remove_ignore_autoload(name)) function_autoloader.unload(name);
}

static const function_info_t *function_get(const wcstring &name) {
    // The caller must lock the functions_lock before calling this; however our mutex is currently
    // recursive, so trylock will never fail. We need a way to correctly check if a lock is locked
    // (or better yet, make our lock non-recursive).
    // ASSERT_IS_LOCKED(functions_lock);
    function_map_t::iterator iter = loaded_functions.find(name);
    if (iter == loaded_functions.end()) {
        return NULL;
    }
    return &iter->second;
}

bool function_get_definition(const wcstring &name, wcstring *out_definition) {
    scoped_rlock locker(functions_lock);
    const function_info_t *func = function_get(name);
    if (func && out_definition) {
        out_definition->assign(func->definition);
    }
    return func != NULL;
}

wcstring_list_t function_get_named_arguments(const wcstring &name) {
    scoped_rlock locker(functions_lock);
    const function_info_t *func = function_get(name);
    return func ? func->named_arguments : wcstring_list_t();
}

std::map<wcstring, env_var_t> function_get_inherit_vars(const wcstring &name) {
    scoped_rlock locker(functions_lock);
    const function_info_t *func = function_get(name);
    return func ? func->inherit_vars : std::map<wcstring, env_var_t>();
}

bool function_get_shadow_scope(const wcstring &name) {
    scoped_rlock locker(functions_lock);
    const function_info_t *func = function_get(name);
    return func ? func->shadow_scope : false;
}

bool function_get_desc(const wcstring &name, wcstring *out_desc) {
    // Empty length string goes to NULL.
    scoped_rlock locker(functions_lock);
    const function_info_t *func = function_get(name);
    if (out_desc && func && !func->description.empty()) {
        out_desc->assign(_(func->description.c_str()));
        return true;
    }

    return false;
}

void function_set_desc(const wcstring &name, const wcstring &desc) {
    load(name);
    scoped_rlock locker(functions_lock);
    function_map_t::iterator iter = loaded_functions.find(name);
    if (iter != loaded_functions.end()) {
        iter->second.description = desc;
    }
}

bool function_copy(const wcstring &name, const wcstring &new_name) {
    bool result = false;
    scoped_rlock locker(functions_lock);
    function_map_t::const_iterator iter = loaded_functions.find(name);
    if (iter != loaded_functions.end()) {
        // This new instance of the function shouldn't be tied to the definition file of the
        // original, so pass NULL filename, etc.
        const function_map_t::value_type new_pair(new_name,
                                                  function_info_t(iter->second, NULL, 0, false));
        loaded_functions.insert(new_pair);
        result = true;
    }
    return result;
}

wcstring_list_t function_get_names(int get_hidden) {
    std::unordered_set<wcstring> names;
    scoped_rlock locker(functions_lock);
    autoload_names(names, get_hidden);

    for (const auto &func : loaded_functions) {
        const wcstring &name = func.first;

        // Maybe skip hidden.
        if (!get_hidden && (name.empty() || name.at(0) == L'_')) {
            continue;
        }
        names.insert(name);
    }
    return wcstring_list_t(names.begin(), names.end());
}

const wchar_t *function_get_definition_file(const wcstring &name) {
    scoped_rlock locker(functions_lock);
    const function_info_t *func = function_get(name);
    return func ? func->definition_file : NULL;
}

bool function_is_autoloaded(const wcstring &name) {
    scoped_rlock locker(functions_lock);
    const function_info_t *func = function_get(name);
    return func->is_autoload;
}

int function_get_definition_offset(const wcstring &name) {
    scoped_rlock locker(functions_lock);
    const function_info_t *func = function_get(name);
    return func ? func->definition_offset : -1;
}

// Setup the environment for the function. There are three components of the environment:
// 1. argv
// 2. named arguments
// 3. inherited variables
void function_prepare_environment(const wcstring &name, const wchar_t *const *argv,
                                  const std::map<wcstring, env_var_t> &inherited_vars) {
    env_set_argv(argv);

    const wcstring_list_t named_arguments = function_get_named_arguments(name);
    if (!named_arguments.empty()) {
        const wchar_t *const *arg = argv;
        for (size_t i = 0; i < named_arguments.size(); i++) {
            if (*arg) {
                env_set_one(named_arguments.at(i), ENV_LOCAL | ENV_USER, *arg);
                arg++;
            } else {
                env_set_empty(named_arguments.at(i), ENV_LOCAL | ENV_USER);
            }
        }
    }

    for (auto it = inherited_vars.begin(), end = inherited_vars.end(); it != end; ++it) {
        env_set(it->first, ENV_LOCAL | ENV_USER, it->second.as_list());
    }
}
