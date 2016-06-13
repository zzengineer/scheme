#include <assert.h>
#include <string.h>
#include "eval.h"
#include "gc.h"
#include "util.h"
#include "intrinsics.h"

#define MAXIMUM_CALL_DEPTH 128
#define MAXIMUM_NATIVE_CALL_ARGS 16

static size_t activation_index = 0;
static activation* activation_stack[MAXIMUM_CALL_DEPTH];

static activation* global_activation;

void scheme_initialize() {
    // initialize the global activation.
    global_activation = gc_allocate_activation();
    activation_initialize(global_activation, NULL);
    activation_stack[activation_index++] = global_activation;

    // set up all intrinsics
    #define INTRINSIC_DEF(scheme_name, arity, c_name, impl) \
        do { \
            sexp proc = gc_allocate_native_proc(arity, c_name, scheme_name); \
            sexp sym = gc_allocate_symbol(scheme_name);                      \
            activation_add_binding(global_activation, sym->symbol_value, proc); \
        } while(0);

    #include "intrinsics.def"
    #undef INTRINSIC_DEF
}

static sexp eval_atom(sexp atom, activation* activation) {
    // all atoms evaluate to themselves except for symbols,
    // which eval to their binding in the activation.
    assert(!sexp_is_cons(atom));

    // procs should never be generated by the reader
    // (they are created by eval'ing lambda forms)
    assert(!sexp_is_proc(atom));
    scheme_symbol sym;
    if (!sexp_extract_symbol(atom, &sym)) {
        return atom;
    }

    sexp result;
    if (!activation_get_binding(activation, sym, &result)) {
        fatal_error("unbound symbol: %s", sym);
    }

    return result;
}

static bool eval_fundamental_form(sexp car, sexp cdr, activation* act, sexp* result) {
    scheme_symbol sym = NULL;
    if (!sexp_extract_symbol(car, &sym)) {
        return false;
    }

    // TODO this is pretty bad. these should all be interned.
    if (strcmp(sym, "define") == 0) {
        // (define <sym> <value>)
        sexp define_sym = NULL;
        sexp define_value = NULL;
        if (!sexp_extract_cons(cdr, &define_sym, &define_value)) {
            fatal_error("invalid define fundamental form");
        }

        scheme_symbol define_name = NULL;
        if (!sexp_extract_symbol(define_sym, &define_name)) {
            fatal_error("first argument to define must be a symbol");
        }

        sexp actual_binding = NULL;
        sexp should_be_empty = NULL;
        if (!sexp_extract_cons(define_value, &actual_binding, &should_be_empty)) {
            fatal_error("invalid define fundamental form");
        }

        if (!sexp_is_empty(should_be_empty)) {
            fatal_error("too many items in define");
        }

        sexp binding_value = scheme_eval(actual_binding, act);
        activation_add_binding(global_activation, define_name, binding_value);
        *result = gc_allocate_empty();
        return true;
    }

    if (strcmp(sym, "let") == 0) {
        // (let ((x 1) (y 2)) <expr>)
        sexp binding_list = NULL;
        sexp body = NULL;
        if (!sexp_extract_cons(cdr, &binding_list, &body)) {
            fatal_error("invalid let fundamental form");
        }

        // binding list is itself a list of two-element lists.
        activation* child_act = gc_allocate_activation();
        activation_initialize(child_act, act);
        activation_stack[activation_index++] = child_act;

        FOR_EACH_LIST(binding_list, binding, {
            // binding is a list of two elements
            sexp binding_name = NULL;
            sexp binding_value = NULL;
            if (!sexp_extract_cons(binding, &binding_name, &binding_value)) {
                fatal_error("invalid let fundamental form");
            }

            scheme_symbol let_sym = NULL;
            if (!sexp_extract_symbol(binding_name, &let_sym)) {
                fatal_error("non-symbol in let binding");
            }

            sexp actual_binding = NULL;
            sexp should_be_empty = NULL;
            if (!sexp_extract_cons(binding_value, &actual_binding, &should_be_empty)) {
                fatal_error("invalid let-binding list");
            }

            if (!sexp_is_empty(should_be_empty)) {
                fatal_error("too many items in let-binding");
            }

            sexp value = scheme_eval(actual_binding, child_act);

            activation_add_binding(child_act, let_sym, value);
        });

        sexp actual_body = NULL;
        sexp should_be_empty = NULL;
        if (!sexp_extract_cons(body, &actual_body, &should_be_empty)) {
            fatal_error("invalid let-binding list");
        }

        if (!sexp_is_empty(should_be_empty)) {
            fatal_error("too many items in let-binding");
        }

        sexp body_result = scheme_eval(actual_body, child_act);
        activation_stack[--activation_index] = NULL;
        *result = body_result;
        return true;
    }

    if (strcmp(sym, "set!") == 0) {
            // (set! <sym> <value>)
        sexp set_sym = NULL;
        sexp set_value = NULL;
        if (!sexp_extract_cons(cdr, &set_sym, &set_value)) {
            fatal_error("invalid set! fundamental form");
        }

        scheme_symbol set_name = NULL;
        if (!sexp_extract_symbol(set_sym, &set_name)) {
            fatal_error("first argument to define must be a symbol");
        }

        sexp actual_value = NULL;
        sexp should_be_empty = NULL;
        if (!sexp_extract_cons(set_value, &actual_value, &should_be_empty)) {
            fatal_error("invalid set! fundamental form");
        }

        if (!sexp_is_empty(should_be_empty)) {
            fatal_error("too many items in set!");
        }

        sexp value = scheme_eval(actual_value, act);

        if (!activation_mutate_binding(act, set_name, value)) {
            fatal_error("unbound symbol: %s", set_name);
        }

        *result = gc_allocate_empty();
        return true;
    }

    if (strcmp(sym, "lambda") == 0) {
        // (lambda <args> <body>)
        sexp arguments = NULL;
        sexp body = NULL;

        if (!sexp_extract_cons(cdr, &arguments, &body)) {
            fatal_error("invalid lambda fundamental form");
        }

        int arity = 0;
        FOR_EACH_LIST(arguments, arg, ((void)arg, arity++));

        sexp actual_body = NULL;
        sexp should_be_empty = NULL;
        if (!sexp_extract_cons(body, &actual_body, &should_be_empty)) {
            fatal_error("invalid lambda fundamental form");
        }

        if (!sexp_is_empty(should_be_empty)) {
            fatal_error("too many items in set!");
        }

        *result = gc_allocate_proc(arity, arguments, actual_body, act);
        return true;
    }

    if (strcmp(sym, "quote") == 0) {
        // quote returns the rest of the list unmodified.
        sexp actual_quote = NULL;
        sexp should_be_empty = NULL;
        if (!sexp_extract_cons(cdr, &actual_quote, &should_be_empty)) {
            fatal_error("invalid quote fundamental form");
        }

        *result = actual_quote;
        return true;
    }

    if (strcmp(sym, "begin") == 0) {
        // (begin <forms>* <final_form>)
        sexp last_result = NULL;
        FOR_EACH_LIST(cdr, form, {
            last_result = scheme_eval(form, act);
        });

        *result = last_result;
        return true;
    }

    return false;
}

static sexp eval_call(sexp function, sexp args, activation* act) {
    // the arity must be an exact match
    assert(sexp_is_proc(function));
    scheme_number arity = 0;
    FOR_EACH_LIST(args, arg, ((void)arg, arity++));
    if (arity != function->arity) {
        fatal_error("called function with wrong arity");
    }

    // for the eval we set up two activations. The outermost one is 
    // the activation of the lambda (for captured variables). The innermost
    // one is for function parameters.
    activation_stack[activation_index++] = function->activation;

    activation* child_act = gc_allocate_activation();
    activation_initialize(child_act, function->activation);
    activation_stack[activation_index++] = child_act;

    FOR_EACH_LIST_2(function->arguments, args, formal_param, actual_param, {
        scheme_symbol param_name = NULL;
        if (!sexp_extract_symbol(formal_param, &param_name)) {
            PANIC("function parameter not a symbol?");
        }

        sexp param_value = scheme_eval(actual_param, act);
        activation_add_binding(child_act, param_name, param_value);
    });

    // now we can eval the body of the lambda.
    sexp lambda_result = scheme_eval(function->body, child_act);

    // one for the parameter actiation
    activation_stack[activation_index--] = NULL;
    // ... and one for the lambda activation
    activation_stack[activation_index--] = NULL;
    return lambda_result;    
}

static sexp eval_native_call(sexp function, sexp args, activation* act) {
    assert(sexp_is_native_proc(function));
    // we don't set up an activation for native calls,
    // since it can't read or write the current environment.

    scheme_number arity = 0;
    FOR_EACH_LIST(args, arg, ((void)arg, arity++));
    if (arity != function->native_arity) {
        fatal_error("called function with wrong arity");
    }

    sexp native_call_args[MAXIMUM_NATIVE_CALL_ARGS];
    size_t idx = 0;
    FOR_EACH_LIST(args, arg, {
        native_call_args[idx++] = scheme_eval(arg, act);
        if (idx == MAXIMUM_NATIVE_CALL_ARGS) {
            fatal_error("too many arguments to native function");
        }
    });

    return function->function_pointer(native_call_args);
}

static sexp eval_list(sexp car, sexp cdr, activation* act) {
    // this is a call of some sort, or it's a fundamental form.
    sexp result;
    if (eval_fundamental_form(car, cdr, act, &result)) {
        return result;
    } 

    sexp function = scheme_eval(car, act);
    if (sexp_is_proc(function)) {
        return eval_call(function, cdr, act);
    }

    if (sexp_is_native_proc(function)) {
        return eval_native_call(function, cdr, act);
    }

    fatal_error("called a non-callable value");
    // unreachable
    return gc_allocate_empty();
}

sexp scheme_global_eval(sexp program) {
    return scheme_eval(program, global_activation);
}

sexp scheme_eval(sexp program, activation* activation) {
    // here we are evaluating a single form.
    if (!sexp_is_cons(program)) {
        // atoms just get eval'd directly, nothing fancy.
        return eval_atom(program, activation);
    }

    sexp car = program->car;
    sexp cdr = program->cdr;

    return eval_list(car, cdr, activation);
}

