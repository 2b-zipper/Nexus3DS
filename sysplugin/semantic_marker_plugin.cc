#include "gcc-plugin.h"
#include "plugin-version.h"
#include "tree.h"
#include "tree-iterator.h"
#include "tree-pretty-print.h"
#include "dumpfile.h"
#include "input.h"
#include "c-family/c-common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

int plugin_is_GPL_compatible;

static std::string g_out_dir;
static FILE *g_out;

static std::string strip_debug_begin_stmt(const std::string &s)
{
    static const char marker[] = "# DEBUG BEGIN STMT;";
    std::string out;
    char quote = 0;
    bool escape = false;
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        if (quote) {
            out.push_back(c);
            if (escape)
                escape = false;
            else if (c == '\\')
                escape = true;
            else if (c == quote)
                quote = 0;
            ++i;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            out.push_back(c);
            ++i;
            continue;
        }
        if (s.compare(i, sizeof(marker) - 1, marker) == 0) {
            i += sizeof(marker) - 1;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

static std::string one_line(std::string s)
{
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = !out.empty();
            continue;
        }
        if (space) {
            out.push_back(' ');
            space = false;
        }
        out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

static std::string pretty(tree t)
{
    if (!t)
        return "";
    char *buf = nullptr;
    size_t size = 0;
    FILE *mem = open_memstream(&buf, &size);
    if (!mem)
        return "";
    print_generic_expr(mem, t, TDF_NOUID);
    fclose(mem);
    std::string s = buf ? std::string(buf, size) : std::string();
    free(buf);
    return one_line(strip_debug_begin_stmt(s));
}

static unsigned long long fnv1a64(const char *text)
{
    unsigned long long h = 1469598103934665603ULL;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

static std::string base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *base = path;
    if (slash && slash + 1 > base)
        base = slash + 1;
    if (backslash && backslash + 1 > base)
        base = backslash + 1;
    std::string out(base);
    for (char &c : out)
        if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '.' && c != '_' && c != '-')
            c = '_';
    return out;
}

static bool ensure_output()
{
    if (g_out)
        return true;
    if (g_out_dir.empty() || !main_input_filename || !*main_input_filename)
        return false;
    char path[4096];
    unsigned long long h = fnv1a64(main_input_filename);
    std::string base = base_name(main_input_filename);
    int n = snprintf(path, sizeof(path), "%s/%016llx-%s.tsv", g_out_dir.c_str(), h, base.c_str());
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(path))
        return false;
    g_out = fopen(path, "w");
    if (!g_out)
        return false;
    fprintf(g_out, "META\tSM3\t%s\t%s\n", gcc_version.basever, main_input_filename);
    return true;
}

static bool loc_parts(location_t loc, std::string &file, int &line, int &col, int &end_line, int &end_col)
{
    if (loc == UNKNOWN_LOCATION)
        return false;
    expanded_location caret = expand_location(loc);
    if (!caret.file || caret.line <= 0)
        return false;
    source_range range = get_range_from_loc(line_table, loc);
    expanded_location finish = expand_location(range.m_finish);
    file = caret.file;
    line = caret.line;
    col = caret.column;
    end_line = finish.line > 0 ? finish.line : caret.line;
    end_col = finish.column > 0 ? finish.column : caret.column;
    return true;
}

static std::string function_name(tree fndecl)
{
    std::string out;
    tree chain[32];
    unsigned count = 0;
    for (tree cur = fndecl; cur && TREE_CODE(cur) == FUNCTION_DECL && count < 32; cur = DECL_CONTEXT(cur))
        chain[count++] = cur;
    for (unsigned i = count; i > 0; --i) {
        tree decl = chain[i - 1];
        const char *name = DECL_NAME(decl) ? IDENTIFIER_POINTER(DECL_NAME(decl)) : "?";
        if (!out.empty())
            out += "/";
        out += name;
    }
    return out.empty() ? "?" : out;
}

static void emit_anchor(const std::string &function, const char *kind, tree loc_tree, const std::string &semantic)
{
    if (!loc_tree || !EXPR_P(loc_tree) || semantic.empty() || !ensure_output())
        return;
    std::string file;
    int line, col, end_line, end_col;
    if (!loc_parts(EXPR_LOCATION(loc_tree), file, line, col, end_line, end_col))
        return;
    fprintf(g_out, "ANCHOR\t%s\t%s\t%d\t%d\t%d\t%d\t%s\t%s\n",
            file.c_str(), function.c_str(), line, col, end_line, end_col, kind, semantic.c_str());
}

static bool usable_location_tree(tree t)
{
    if (!t || !EXPR_P(t))
        return false;
    std::string file;
    int line, col, end_line, end_col;
    return loc_parts(EXPR_LOCATION(t), file, line, col, end_line, end_col);
}

static tree first_usable_location(tree a, tree b = nullptr, tree c = nullptr)
{
    if (usable_location_tree(a)) return a;
    if (usable_location_tree(b)) return b;
    if (usable_location_tree(c)) return c;
    return nullptr;
}

static void scan_stmt(tree t, const std::string &fn);

static void scan_container(tree t, const std::string &fn)
{
    if (!t)
        return;
    enum tree_code code = TREE_CODE(t);
    if (code == BIND_EXPR) {
        scan_container(BIND_EXPR_BODY(t), fn);
        return;
    }
    if (code == STATEMENT_LIST) {
        for (tree_stmt_iterator it = tsi_start(t); !tsi_end_p(it); tsi_next(&it))
            scan_stmt(tsi_stmt(it), fn);
        return;
    }
    scan_stmt(t, fn);
}

static void scan_stmt(tree t, const std::string &fn)
{
    if (!t)
        return;
    enum tree_code code = TREE_CODE(t);
    if (code == DEBUG_BEGIN_STMT)
        return;
    if (code == BIND_EXPR || code == STATEMENT_LIST) {
        scan_container(t, fn);
        return;
    }
    if (code == COND_EXPR && TREE_TYPE(t) == void_type_node) {
        tree cond = COND_EXPR_COND(t);
        emit_anchor(fn, "if", first_usable_location(cond, t), pretty(cond));
        scan_container(COND_EXPR_THEN(t), fn);
        scan_container(COND_EXPR_ELSE(t), fn);
        return;
    }
    if (code == IF_STMT) {
        tree cond = TREE_OPERAND(t, 0);
        emit_anchor(fn, "if", first_usable_location(cond, t), pretty(cond));
        scan_container(TREE_OPERAND(t, 1), fn);
        scan_container(TREE_OPERAND(t, 2), fn);
        return;
    }
    if (code == WHILE_STMT) {
        tree cond = WHILE_COND(t);
        emit_anchor(fn, "while", first_usable_location(cond, t), pretty(cond));
        scan_container(WHILE_BODY(t), fn);
        return;
    }
    if (code == FOR_STMT) {
        tree cond = FOR_COND(t);
        tree step = FOR_EXPR(t);
        std::string sem = "init=" + pretty(FOR_INIT_STMT(t)) + ";cond=" + pretty(cond) + ";step=" + pretty(step);
        emit_anchor(fn, "for", first_usable_location(step, cond, t), sem);
        scan_container(FOR_BODY(t), fn);
        return;
    }
    if (code == DO_STMT) {
        tree cond = DO_COND(t);
        emit_anchor(fn, "do_while", first_usable_location(cond, t), pretty(cond));
        scan_container(DO_BODY(t), fn);
        return;
    }
    if (code == RANGE_FOR_STMT) {
        tree decl = TREE_OPERAND(t, 0);
        tree expr = TREE_OPERAND(t, 1);
        tree body = TREE_OPERAND(t, 2);
        tree init = TREE_OPERAND(t, 5);
        std::string sem = "decl=" + pretty(decl) + ";expr=" + pretty(expr) + ";init=" + pretty(init);
        emit_anchor(fn, "range_for", first_usable_location(expr, decl, t), sem);
        scan_container(body, fn);
        return;
    }
    if (code == TRY_BLOCK) {
        scan_container(TREE_OPERAND(t, 0), fn);
        scan_container(TREE_OPERAND(t, 1), fn);
        return;
    }
    if (code == HANDLER) {
        scan_container(TREE_OPERAND(t, 1), fn);
        return;
    }
    if (code == CLEANUP_STMT) {
        scan_container(TREE_OPERAND(t, 0), fn);
        return;
    }
    if (code == SWITCH_STMT) {
        emit_anchor(fn, "switch", t, pretty(SWITCH_STMT_COND(t)));
        scan_container(SWITCH_STMT_BODY(t), fn);
        return;
    }
    if (EXPR_P(t))
        emit_anchor(fn, get_tree_code_name(code), t, pretty(t));
    if (code == TRY_FINALLY_EXPR || code == TRY_CATCH_EXPR) {
        scan_container(TREE_OPERAND(t, 0), fn);
        scan_container(TREE_OPERAND(t, 1), fn);
    }
}

static void pre_genericize(void *gcc_data, void *)
{
    tree fndecl = static_cast<tree>(gcc_data);
    if (!fndecl || TREE_CODE(fndecl) != FUNCTION_DECL)
        return;
    scan_container(DECL_SAVED_TREE(fndecl), function_name(fndecl));
    if (g_out)
        fflush(g_out);
}

static void finish_unit(void *, void *)
{
    if (g_out) {
        fclose(g_out);
        g_out = nullptr;
    }
}

int plugin_init(plugin_name_args *info, plugin_gcc_version *version)
{
    if (!plugin_default_version_check(version, &gcc_version))
        return 1;
    for (int i = 0; i < info->argc; ++i)
        if (strcmp(info->argv[i].key, "out-dir") == 0 && info->argv[i].value)
            g_out_dir = info->argv[i].value;
    if (g_out_dir.empty())
        return 1;
    register_callback(info->base_name, PLUGIN_PRE_GENERICIZE, pre_genericize, nullptr);
    register_callback(info->base_name, PLUGIN_FINISH_UNIT, finish_unit, nullptr);
    return 0;
}
