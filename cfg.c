#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfg.h"

static char *dupstr(const char *s) {
    return s ? strdup(s) : NULL;
}
static CFGNode *new_node(FunctionCFG *f, CFGNodeKind kind, AST *op, const char *label) {
    CFGNode *n = calloc(1, sizeof(*n));
    n->id = f->nnodes;
    n->kind = kind;
    n->operation = op;
    n->optree = optree_from_ast(op);
    n->label_override = dupstr(label);
    f->nodes = realloc(f->nodes, (f->nnodes + 1) * sizeof(*f->nodes));
    f->nodes[f->nnodes++] = n;
    return n;
}
static void error(AnalysisResult *r, const char *s) {
    r->errors = realloc(r->errors, (r->nerrors + 1) * sizeof(*r->errors));
    r->errors[r->nerrors++] = strdup(s);
}
static CFGNode *lower(AnalysisResult *r, FunctionCFG *f, AST *s, CFGNode *next, CFGNode *break_to,
                      CFGNode *continue_to) {
    CFGNode *n;
    if (!s)
        return next;
    switch (s->type) {
    case NODE_BLOCK:
    case NODE_PARAM_LIST:
        for (int i = s->nchildren - 1; i >= 0; i--)
            next = lower(r, f, s->children[i], next, break_to, continue_to);
        return next;
    case NODE_IF:
        n = new_node(f, CFG_CONDITION, s->children[0], "if");
        n->nextDefault = lower(r, f, s->children[1], next, break_to, continue_to);
        n->nextConditional =
            s->nchildren > 2 ? lower(r, f, s->children[2], next, break_to, continue_to) : next;
        n->condLabel = strdup("true");
        return n;
    case NODE_WHILE:
        n = new_node(f, CFG_CONDITION, s->children[0], "while");
        n->nextDefault = lower(r, f, s->children[1], n, next, n);
        n->nextConditional = next;
        n->condLabel = strdup("true");
        return n;
    case NODE_DO_WHILE: {
        n = new_node(f, CFG_CONDITION, s->children[1], "do while");
        CFGNode *body = lower(r, f, s->children[0], n, next, n);
        n->nextDefault = body;
        n->nextConditional = next;
        n->condLabel = strdup("true");
        return body;
    }
    case NODE_BREAK:
    case NODE_CONTINUE:
        n = new_node(f, CFG_NOP, NULL, s->label);
        n->nextDefault = s->type == NODE_BREAK ? break_to : continue_to;
        if (!n->nextDefault)
            error(r, s->type == NODE_BREAK ? "break outside loop" : "continue outside loop");
        return n;
    case NODE_RETURN:
        return new_node(f, CFG_RETURN, s->nchildren ? s->children[0] : NULL, "return");
    case NODE_VAR_DECL:
        n = new_node(f, CFG_DECL, s, "declaration");
        break;
    case NODE_EXPR_STMT:
        n = new_node(f, CFG_EXPR, s->children[0], "expression");
        break;
    default:
        error(r, "unsupported statement in CFG");
        return next;
    }
    n->nextDefault = next;
    return n;
}
static void add_function(AnalysisResult *r, AST *a, const char *source_file) {
    FunctionCFG *f = calloc(1, sizeof(*f));
    f->name = strdup(a->label);
    f->signature = a->children[0];
    f->source_file = strdup(source_file ? source_file : "");
    f->has_body = a->nchildren > 1;
    r->functions = realloc(r->functions, (r->nfunctions + 1) * sizeof(*r->functions));
    r->functions[r->nfunctions++] = f;
    if (f->has_body) {
        f->exit = new_node(f, CFG_NOP, NULL, "exit");
        f->entry = lower(r, f, a->children[1], f->exit, NULL, NULL);
    }
}
AnalysisResult *build_cfg_from_ast(AST *root, const char *source_file) {
    AnalysisResult *r = calloc(1, sizeof(*r));
    if (!root)
        return r;
    for (int i = 0; i < root->nchildren; i++)
        if (root->children[i]->type == NODE_FUNCTION)
            add_function(r, root->children[i], source_file);
    return r;
}

void free_analysis(AnalysisResult *res) {
    if (!res)
        return;
    for (int i = 0; i < res->nfunctions; i++) {
        FunctionCFG *fn = res->functions[i];
        if (!fn)
            continue;
        for (int j = 0; j < fn->nnodes; j++) {
            CFGNode *n = fn->nodes[j];
            if (!n)
                continue;
            if (n->optree)
                optree_free(n->optree);
            if (n->label_override)
                free(n->label_override);
            if (n->condLabel)
                free(n->condLabel);
            free(n);
        }
        free(fn->nodes);
        free(fn->name);
        free(fn->source_file);
        free(fn);
    }
    for (int e = 0; e < res->nerrors; e++)
        free(res->errors[e]);
    free(res->errors);
    free(res->functions);
    free(res);
}

static char *escape_for_dot(const char *s) {
    if (!s)
        return dupstr("");
    size_t len = strlen(s);
    size_t cap = len * 4 + 1;
    char *esc = malloc(cap);
    char *q = esc;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\\') {
            *q++ = '\\';
            *q++ = '\\';
        } else if (c == '"') {
            *q++ = '\\';
            *q++ = '"';
        } else if (c == '\n') {
            *q++ = '\\';
            *q++ = 'n';
        } else if (c == '\r') {
            *q++ = '\\';
            *q++ = 'r';
        } else if (c == '\t') {
            *q++ = '\\';
            *q++ = 't';
        } else
            *q++ = c;
    }
    *q = '\0';
    return esc;
}

static char *render_optree_cluster(FILE *f, OpNode *node, int cfgid, int *local_counter) {
    if (!node) {
        int id = (*local_counter)++;
        char *name = malloc(64);
        snprintf(name, 64, "n%d_a%d", cfgid, id);
        fprintf(f, "    %s [label=\"node\",shape=oval];\n", name);
        return name;
    }

    int myid = (*local_counter)++;
    char *myname = malloc(64);
    snprintf(myname, 64, "n%d_a%d", cfgid, myid);

    char oplab[256];
    oplab[0] = '\0';

    switch (node->type) {
    case OP_LITERAL:
        if (node->value)
            snprintf(oplab, sizeof(oplab), "%s", node->value);
        else
            strcpy(oplab, "literal");
        break;
    case OP_IDENTIFIER:
        if (node->value)
            snprintf(oplab, sizeof(oplab), "%s", node->value);
        else
            strcpy(oplab, "var");
        break;
    case OP_BINARY:
        if (node->op)
            snprintf(oplab, sizeof(oplab), "%s", node->op);
        else
            strcpy(oplab, "op");
        break;
    case OP_UNARY:
        if (node->op)
            snprintf(oplab, sizeof(oplab), "%s", node->op);
        else
            strcpy(oplab, "unary");
        break;
    case OP_CALL:
        if (node->op)
            snprintf(oplab, sizeof(oplab), "call %s", node->op);
        else
            strcpy(oplab, "call");
        break;
    case OP_INDEX:
        strcpy(oplab, "[]");
        break;
    case OP_ARRAY_TYPE:
        if (node->value)
            snprintf(oplab, sizeof(oplab), "%s", node->value);
        else
            strcpy(oplab, "type");
        break;
    }

    char *esc_oplab = escape_for_dot(oplab);
    fprintf(f, "    %s [label=\"%s\",shape=oval];\n", myname, esc_oplab);
    free(esc_oplab);

    for (int i = 0; i < node->nchildren; i++) {
        char *childname = render_optree_cluster(f, node->children[i], cfgid, local_counter);
        fprintf(f, "    %s -> %s;\n", myname, childname);
        free(childname);
    }

    return myname;
}

static int optree_is_empty(OpNode *node) {
    if (!node)
        return 1;
    return 0;
}

static char *resolve_repname(char **rep, FunctionCFG *fng, CFGNode *node, int depth) {
    if (!node)
        return "end";
    if (depth > fng->nnodes + 5)
        return "end";
    if (rep[node->id])
        return rep[node->id];

    if (node->nextDefault) {
        char *r = resolve_repname(rep, fng, node->nextDefault, depth + 1);
        if (r && strcmp(r, "end") != 0)
            return r;
    }
    if (node->nextConditional) {
        char *r = resolve_repname(rep, fng, node->nextConditional, depth + 1);
        if (r && strcmp(r, "end") != 0)
            return r;
    }

    if (fng->exit && fng->exit != node && rep[fng->exit->id])
        return rep[fng->exit->id];
    return "end";
}

void write_function_dot(FILE *f, FunctionCFG *fn) {
    fprintf(f, "digraph \"%s\" {\n", fn->name);
    fprintf(f, "  rankdir=LR;\n");
    fprintf(f, "  start [shape=oval,label=\"START\"];\n");
    fprintf(f, "  end [shape=oval,label=\"END\"];\n");

    char **repname = calloc(fn->nnodes, sizeof(char *));
    for (int i = 0; i < fn->nnodes; i++) {
        CFGNode *n = fn->nodes[i];
        if (optree_is_empty(n->optree) && !n->label_override) {
            repname[n->id] = NULL;
            continue;
        }

        fprintf(f, "  subgraph cluster_%d {\n", n->id);
        fprintf(f, "    style=rounded;\n");
        char *labelname = malloc(64);
        snprintf(labelname, 64, "expr_%d_label", n->id);
        const char *ctitle = NULL;
        if (n->label_override)
            ctitle = n->label_override;
        else {
            ctitle = "EXPRESSION";
        }
        char *esc_ctitle = escape_for_dot(ctitle);
        fprintf(f, "    %s [label=\"%s\",shape=oval];\n", labelname, esc_ctitle);
        free(esc_ctitle);
        int local = 0;
        char *rootname = render_optree_cluster(f, n->optree, n->id, &local);
        if (rootname) {
            fprintf(f, "    %s -> %s;\n", labelname, rootname);
            free(rootname);
        }
        fprintf(f, "  }\n");
        repname[n->id] = labelname;
    }

    if (fn->entry) {
        char *entry_rep = resolve_repname(repname, fn, fn->entry, 0);
        fprintf(f, "  start -> %s;\n", entry_rep);
    }

    for (int i = 0; i < fn->nnodes; i++) {
        CFGNode *n = fn->nodes[i];
        if (n->kind == CFG_RETURN) {
            char *from = resolve_repname(repname, fn, n, 0);
            fprintf(f, "  %s -> end;\n", from);
        }
    }

    for (int i = 0; i < fn->nnodes; i++) {
        CFGNode *n = fn->nodes[i];

        if (n->nextDefault) {
            char *from = resolve_repname(repname, fn, n, 0);
            char *to = resolve_repname(repname, fn, n->nextDefault, 0);
            if (strcmp(from, to) != 0) {
                if (n->condLabel && strcmp(n->condLabel, "true") == 0) {
                    fprintf(f, "  %s -> %s [label=\"true\", color=green];\n", from, to);
                } else {
                    fprintf(f, "  %s -> %s;\n", from, to);
                }
            }
        }

        if (n->nextConditional) {
            char *from = resolve_repname(repname, fn, n, 0);
            char *to = resolve_repname(repname, fn, n->nextConditional, 0);
            if (strcmp(from, to) != 0) {
                fprintf(f, "  %s -> %s [label=\"false\", color=red];\n", from, to);
            }
        }
    }

    if (fn->exit) {
        char *exit_rep = resolve_repname(repname, fn, fn->exit, 0);
        if (exit_rep && strcmp(exit_rep, "end") != 0) {
            fprintf(f, "  %s -> end;\n", exit_rep);
        }
    }

    for (int i = 0; i < fn->nnodes; i++)
        free(repname[i]);
    free(repname);

    fprintf(f, "}\n");
}

static void visit_optree_calls(OpNode *node, AnalysisResult *res, int caller_index, FILE *f) {
    if (!node)
        return;
    if (node->type == OP_CALL) {
        if (node->op) {
            const char *called = node->op;
            for (int k = 0; k < res->nfunctions; k++) {
                if (strcmp(res->functions[k]->name, called) == 0) {
                    fprintf(f, "  f%d -> f%d;\n", caller_index, k);
                }
            }
        }
    }
    for (int i = 0; i < node->nchildren; i++) {
        visit_optree_calls(node->children[i], res, caller_index, f);
    }
}

void write_callgraph_dot(FILE *f, AnalysisResult *res) {
    fprintf(f, "digraph callgraph {\n");
    for (int i = 0; i < res->nfunctions; i++) {
        fprintf(f, "  f%d [label=\"%s\"];\n", i, res->functions[i]->name);
    }
    for (int i = 0; i < res->nfunctions; i++) {
        FunctionCFG *fn = res->functions[i];
        for (int j = 0; j < fn->nnodes; j++) {
            CFGNode *n = fn->nodes[j];
            if (!n->optree)
                continue;
            visit_optree_calls(n->optree, res, i, f);
        }
    }
    fprintf(f, "}\n");
}
