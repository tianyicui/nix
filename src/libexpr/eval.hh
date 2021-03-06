#ifndef __EVAL_H
#define __EVAL_H

#include "nixexpr.hh"
#include "symbol-table.hh"
#include "hash.hh"

#include <map>

#if HAVE_BOEHMGC
#include <gc/gc_allocator.h>
#endif


namespace nix {


class EvalState;
struct Env;
struct Value;
struct Attr;


/* Attribute sets are represented as a vector of attributes, sorted by
   symbol (i.e. pointer to the attribute name in the symbol table). */
#if HAVE_BOEHMGC
typedef std::vector<Attr, gc_allocator<Attr> > BindingsBase;
#else
typedef std::vector<Attr> BindingsBase;
#endif


class Bindings : public BindingsBase
{
public:
    iterator find(const Symbol & name);
    void sort();
};


typedef enum {
    tInt = 1,
    tBool,
    tString,
    tPath,
    tNull,
    tAttrs,
    tList,
    tThunk,
    tApp,
    tLambda,
    tBlackhole,
    tPrimOp,
    tPrimOpApp,
} ValueType;


typedef void (* PrimOpFun) (EvalState & state, Value * * args, Value & v);


struct PrimOp
{
    PrimOpFun fun;
    unsigned int arity;
    Symbol name;
    PrimOp(PrimOpFun fun, unsigned int arity, Symbol name)
        : fun(fun), arity(arity), name(name) { }
};


struct Value
{
    ValueType type;
    union 
    {
        int integer;
        bool boolean;
        
        /* Strings in the evaluator carry a so-called `context' (the
           ATermList) which is a list of strings representing store
           paths.  This is to allow users to write things like

             "--with-freetype2-library=" + freetype + "/lib"

           where `freetype' is a derivation (or a source to be copied
           to the store).  If we just concatenated the strings without
           keeping track of the referenced store paths, then if the
           string is used as a derivation attribute, the derivation
           will not have the correct dependencies in its inputDrvs and
           inputSrcs.

           The semantics of the context is as follows: when a string
           with context C is used as a derivation attribute, then the
           derivations in C will be added to the inputDrvs of the
           derivation, and the other store paths in C will be added to
           the inputSrcs of the derivations.

           For canonicity, the store paths should be in sorted order. */
        struct {
            const char * s;
            const char * * context; // must be in sorted order
        } string;
        
        const char * path;
        Bindings * attrs;
        struct {
            unsigned int length;
            Value * * elems;
        } list;
        struct {
            Env * env;
            Expr * expr;
        } thunk;
        struct {
            Value * left, * right;
        } app;
        struct {
            Env * env;
            ExprLambda * fun;
        } lambda;
        PrimOp * primOp;
        struct {
            Value * left, * right;
        } primOpApp;
    };
};


struct Env
{
    Env * up;
    unsigned int prevWith; // nr of levels up to next `with' environment
    Value * values[0];
};


struct Attr
{
    Symbol name;
    Value * value;
    Pos * pos;
    Attr(Symbol name, Value * value, Pos * pos = &noPos)
        : name(name), value(value), pos(pos) { };
    Attr() : pos(&noPos) { };
    bool operator < (const Attr & a) const
    {
        return name < a.name;
    }
};


/* After overwriting an app node, be sure to clear pointers in the
   Value to ensure that the target isn't kept alive unnecessarily. */
static inline void clearValue(Value & v)
{
    v.app.right = 0;
}


static inline void mkInt(Value & v, int n)
{
    clearValue(v);
    v.type = tInt;
    v.integer = n;
}


static inline void mkBool(Value & v, bool b)
{
    clearValue(v);
    v.type = tBool;
    v.boolean = b;
}


static inline void mkApp(Value & v, Value & left, Value & right)
{
    v.type = tApp;
    v.app.left = &left;
    v.app.right = &right;
}


void mkString(Value & v, const char * s);
void mkString(Value & v, const string & s, const PathSet & context = PathSet());
void mkPath(Value & v, const char * s);

void copyContext(const Value & v, PathSet & context);


typedef std::map<Path, Hash> DrvHashes;

/* Cache for calls to addToStore(); maps source paths to the store
   paths. */
typedef std::map<Path, Path> SrcToStore;

struct EvalState;


std::ostream & operator << (std::ostream & str, const Value & v);


class EvalState 
{
public:
    DrvHashes drvHashes; /* normalised derivation hashes */

    SymbolTable symbols;

    const Symbol sWith, sOutPath, sDrvPath, sType, sMeta, sName,
        sSystem, sOverrides;

private:
    SrcToStore srcToStore; 

    bool allowUnsafeEquality;

    std::map<Path, Expr *> parseTrees;

public:
    
    EvalState();
    ~EvalState();

    /* Evaluate an expression read from the given file to normal
       form. */
    void evalFile(const Path & path, Value & v);

    /* Evaluate an expression to normal form, storing the result in
       value `v'. */
    void eval(Expr * e, Value & v);
    void eval(Env & env, Expr * e, Value & v);

    /* Evaluation the expression, then verify that it has the expected
       type. */
    bool evalBool(Env & env, Expr * e);
    void evalAttrs(Env & env, Expr * e, Value & v);

    /* If `v' is a thunk, enter it and overwrite `v' with the result
       of the evaluation of the thunk.  If `v' is a delayed function
       application, call the function and overwrite `v' with the
       result.  Otherwise, this is a no-op. */
    void forceValue(Value & v);

    /* Force a value, then recursively force list elements and
       attributes. */
    void strictForceValue(Value & v);

    /* Force `v', and then verify that it has the expected type. */
    int forceInt(Value & v);
    bool forceBool(Value & v);
    void forceAttrs(Value & v);
    void forceList(Value & v);
    void forceFunction(Value & v); // either lambda or primop
    string forceString(Value & v);
    string forceString(Value & v, PathSet & context);
    string forceStringNoCtx(Value & v);

    /* Return true iff the value `v' denotes a derivation (i.e. a
       set with attribute `type = "derivation"'). */
    bool isDerivation(Value & v);

    /* String coercion.  Converts strings, paths and derivations to a
       string.  If `coerceMore' is set, also converts nulls, integers,
       booleans and lists to a string.  If `copyToStore' is set,
       referenced paths are copied to the Nix store as a side effect.q */
    string coerceToString(Value & v, PathSet & context,
        bool coerceMore = false, bool copyToStore = true);

    /* Path coercion.  Converts strings, paths and derivations to a
       path.  The result is guaranteed to be a canonicalised, absolute
       path.  Nothing is copied to the store. */
    Path coerceToPath(Value & v, PathSet & context);

private:

    /* The base environment, containing the builtin functions and
       values. */
    Env & baseEnv;

    unsigned int baseEnvDispl;

public:
    
    /* The same, but used during parsing to resolve variables. */
    StaticEnv staticBaseEnv; // !!! should be private

private:
    
    void createBaseEnv();
    
    void addConstant(const string & name, Value & v);

    void addPrimOp(const string & name,
        unsigned int arity, PrimOpFun primOp);

    Value * lookupVar(Env * env, const VarRef & var);
    
    friend class ExprVar;
    friend class ExprAttrs;
    friend class ExprLet;

public:
    
    /* Do a deep equality test between two values.  That is, list
       elements and attributes are compared recursively. */
    bool eqValues(Value & v1, Value & v2);

    void callFunction(Value & fun, Value & arg, Value & v);

    /* Automatically call a function for which each argument has a
       default value or has a binding in the `args' map. */
    void autoCallFunction(Bindings & args, Value & fun, Value & res);
    
    /* Allocation primitives. */
    Value * allocValue();
    Env & allocEnv(unsigned int size);

    Value * allocAttr(Value & vAttrs, const Symbol & name);

    void mkList(Value & v, unsigned int length);
    void mkAttrs(Value & v, unsigned int expected);
    void mkThunk_(Value & v, Expr * expr);

    Value * maybeThunk(Env & env, Expr * expr);
    
    /* Print statistics. */
    void printStats();

private:
    
    unsigned long nrEnvs;
    unsigned long nrValuesInEnvs;
    unsigned long nrValues;
    unsigned long nrListElems;
    unsigned long nrEvaluated;
    unsigned long nrAttrsets;
    unsigned long nrOpUpdates;
    unsigned long nrOpUpdateValuesCopied;
    unsigned int recursionDepth;
    unsigned int maxRecursionDepth;
    char * deepestStack; /* for measuring stack usage */
    
    friend class RecursionCounter;
    friend class ExprOpUpdate;
};


/* Return a string representing the type of the value `v'. */
string showType(const Value & v);


}


#endif /* !__EVAL_H */
