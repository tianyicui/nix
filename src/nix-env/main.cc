#include "globals.hh"
#include "normalise.hh"
#include "shared.hh"
#include "parser.hh"
#include "eval.hh"


typedef void (* Operation) (EvalState & state,
    Strings opFlags, Strings opArgs);


struct DrvInfo
{
    string name;
    Path drvPath;
    Path outPath;
};

typedef map<string, DrvInfo> DrvInfos;


bool parseDerivation(EvalState & state, Expr e, DrvInfo & drv)
{
    ATMatcher m;
    
    e = evalExpr(state, e);
    if (!(atMatch(m, e) >> "Attrs")) return false;
    Expr a = queryAttr(e, "type");
    if (!a || evalString(state, a) != "derivation") return false;

    a = queryAttr(e, "name");
    if (!a) throw badTerm("derivation name missing", e);
    drv.name = evalString(state, a);

    a = queryAttr(e, "drvPath");
    if (!a) throw badTerm("derivation path missing", e);
    drv.drvPath = evalPath(state, a);

    a = queryAttr(e, "outPath");
    if (!a) throw badTerm("output path missing", e);
    drv.outPath = evalPath(state, a);

    return true;
}


bool parseDerivations(EvalState & state, Expr e, DrvInfos & drvs)
{
    e = evalExpr(state, e);
    
    ATermMap drvMap;
    queryAllAttrs(e, drvMap);

    for (ATermIterator i(drvMap.keys()); i; ++i) {
        DrvInfo drv;
        debug(format("evaluating attribute `%1%'") % *i);
        if (parseDerivation(state, drvMap.get(*i), drv))
            drvs[drv.name] = drv;
    }

    return true;
}


void loadDerivations(EvalState & state, Path nePath, DrvInfos & drvs)
{
    Expr e = parseExprFromFile(absPath(nePath));
    if (!parseDerivations(state, e, drvs))
        throw badTerm("expected set of derivations", e);
}


static Path getLinksDir()
{
    return canonPath(nixStateDir + "/links");
}


Path createLink(Path outPath, Path drvPath)
{
    Path linksDir = getLinksDir();

    unsigned int num = 0;
    
    Strings names = readDirectory(linksDir);
    for (Strings::iterator i = names.begin(); i != names.end(); ++i) {
        istringstream s(*i);
        unsigned int n; 
        if (s >> n && s.eof() && n > num) num = n + 1;
    }

    Path linkPath = (format("%1%/%2%") % linksDir % num).str();

    if (symlink(outPath.c_str(), linkPath.c_str()) != 0)
        throw SysError(format("creating symlink `%1%'") % linkPath);

    return linkPath;
}


void installDerivations(EvalState & state,
    Path nePath, Strings drvNames)
{
    debug(format("installing derivations from `%1%'") % nePath);

    /* Fetch all derivations from the input file. */
    DrvInfos availDrvs;
    loadDerivations(state, nePath, availDrvs);

    /* Filter out the ones we're not interested in. */
    DrvInfos selectedDrvs;
    for (Strings::iterator i = drvNames.begin();
         i != drvNames.end(); ++i)
    {
        DrvInfos::iterator j = availDrvs.find(*i);
        if (j == availDrvs.end())
            throw Error(format("unknown derivation `%1%'") % *i);
        else
            selectedDrvs[j->first] = j->second;
    }

    /* Get the environment builder expression. */
    Expr envBuilder = parseExprFromFile("/home/eelco/nix/corepkgs/buildenv"); /* !!! */

    /* Construct the whole top level derivation. */
    ATermList inputs = ATempty;
    for (DrvInfos::iterator i = selectedDrvs.begin();
         i != selectedDrvs.end(); ++i)
    {
        ATerm t = ATmake(
            "Attrs(["
            "Bind(\"type\", Str(\"derivation\")), "
            "Bind(\"name\", Str(<str>)), "
            "Bind(\"drvPath\", Path(<str>)), "
            "Bind(\"outPath\", Path(<str>))"
            "])",
            i->second.name.c_str(),
            i->second.drvPath.c_str(),
            i->second.outPath.c_str());
        inputs = ATinsert(inputs, t);
    }

    ATerm inputs2 = ATmake("List(<term>)", ATreverse(inputs));

    /* Also write a copy of the list of inputs to the store; we need
       it for future modifications of the environment. */
    Path inputsFile = writeTerm(inputs2, "-env-inputs");

    Expr topLevel = ATmake(
        "Call(<term>, Attrs(["
        "Bind(\"system\", Str(<str>)), "
        "Bind(\"derivations\", <term>), " // !!! redundant
        "Bind(\"manifest\", Path(<str>))"
        "]))",
        envBuilder, thisSystem.c_str(), inputs2, inputsFile.c_str());

    /* Instantiate it. */
    debug(format("evaluating builder expression `%1%'") % topLevel);
    DrvInfo topLevelDrv;
    if (!parseDerivation(state, topLevel, topLevelDrv))
        abort();
    
    /* Realise the resulting store expression. */
    debug(format("realising user environment"));
    Path nfPath = normaliseStoreExpr(topLevelDrv.drvPath);
    realiseClosure(nfPath);

    /* Switch the current user environment to the output path. */
    debug(format("switching to new user environment"));
    Path linkPath = createLink(topLevelDrv.outPath, topLevelDrv.drvPath);
//     switchLink(current"), link);
}


static void opInstall(EvalState & state,
    Strings opFlags, Strings opArgs)
{
    if (opArgs.size() < 1) throw UsageError("Nix expression expected");

    Path nePath = opArgs.front();
    opArgs.pop_front();

    installDerivations(state, nePath,
        Strings(opArgs.begin(), opArgs.end()));
}


static void opQuery(EvalState & state,
    Strings opFlags, Strings opArgs)
{
    enum { qName } query = qName;
    enum { sInstalled, sAvailable } source = sInstalled;

    for (Strings::iterator i = opFlags.begin();
         i != opFlags.end(); ++i)
        if (*i == "--name") query = qName;
        else if (*i == "--installed") source = sInstalled;
        else if (*i == "--available" || *i == "-f") source = sAvailable;
        else throw UsageError(format("unknown flag `%1%'") % *i);

    /* Obtain derivation information from the specified source. */
    DrvInfos drvs;

    switch (source) {

        case sInstalled:
            break;

        case sAvailable: {
            Path nePath = opArgs.front();
            opArgs.pop_front();
            loadDerivations(state, nePath, drvs);
            break;
        }

        default: abort();
    }

    /* Perform the specified query on the derivations. */
    switch (query) {

        case qName: {
            if (opArgs.size() != 0) throw UsageError("no arguments expected");
            for (DrvInfos::iterator i = drvs.begin(); i != drvs.end(); ++i)
                cout << format("%1%\n") % i->second.name;
            break;
        }
        
        default: abort();
    }
}


void run(Strings args)
{
    EvalState state;
    Strings opFlags, opArgs;
    Operation op = 0;

    for (Strings::iterator i = args.begin(); i != args.end(); ++i) {
        string arg = *i;

        Operation oldOp = op;

        if (arg == "--install" || arg == "-i")
            op = opInstall;
        if (arg == "--query" || arg == "-q")
            op = opQuery;
        else if (arg == "--verbose" || arg == "-v")
            verbosity = (Verbosity) ((int) verbosity + 1);
        else if (arg[0] == '-')
            opFlags.push_back(arg);
        else
            opArgs.push_back(arg);

        if (oldOp && oldOp != op)
            throw UsageError("only one operation may be specified");
    }

    if (!op) throw UsageError("no operation specified");

    openDB();

    op(state, opFlags, opArgs);

    printEvalStats(state);
}


string programId = "nix-env";
