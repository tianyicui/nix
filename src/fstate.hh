#ifndef __FSTATE_H
#define __FSTATE_H

extern "C" {
#include <aterm2.h>
}

#include "store.hh"


/* Abstract syntax of fstate-expressions. */

typedef list<FSId> FSIds;

struct SliceElem
{
    FSId id;
    StringSet refs;
};

typedef map<string, SliceElem> SliceElems;

struct Slice
{
    StringSet roots;
    SliceElems elems;
};

typedef map<string, FSId> DeriveOutputs;
typedef map<string, string> StringPairs;

struct Derive
{
    DeriveOutputs outputs;
    FSIdSet inputs;
    string platform;
    string builder;
    Strings args;
    StringPairs env;
};

struct FState
{
    enum { fsSlice, fsDerive } type;
    Slice slice;
    Derive derive;
};


/* Return a canonical textual representation of an expression. */
string printTerm(ATerm t);

/* Throw an exception with an error message containing the given
   aterm. */
Error badTerm(const format & f, ATerm t);

/* Hash an aterm. */
Hash hashTerm(ATerm t);

/* Read an aterm from disk, given its id. */
ATerm termFromId(const FSId & id);

/* Write an aterm to the Nix store directory, and return its hash. */
FSId writeTerm(ATerm t, const string & suffix, FSId id = FSId());

/* Parse an fstate-expression. */
FState parseFState(ATerm t);

/* Parse an fstate-expression. */
ATerm unparseFState(const FState & fs);


#endif /* !__FSTATE_H */
