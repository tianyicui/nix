#include <map>
#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "build.hh"
#include "references.hh"
#include "pathlocks.hh"
#include "globals.hh"


/* !!! TODO derivationFromPath shouldn't be used here */


static string pathNullDevice = "/dev/null";


/* Forward definition. */
class Worker;


/* A pointer to a goal. */
class Goal;
typedef shared_ptr<Goal> GoalPtr;
typedef weak_ptr<Goal> WeakGoalPtr;

/* Set of goals. */
typedef set<GoalPtr> Goals;
typedef set<WeakGoalPtr> WeakGoals;

/* A map of paths to goals (and the other way around). */
typedef map<Path, WeakGoalPtr> WeakGoalMap;



class Goal : public enable_shared_from_this<Goal>
{
protected:
    
    /* Backlink to the worker. */
    Worker & worker;

    /* Goals that this goal is waiting for. */
    Goals waitees;

    /* Goals waiting for this one to finish.  Must use weak pointers
       here to prevent cycles. */
    WeakGoals waiters;

    /* Number of goals we are/were waiting for that have failed. */
    unsigned int nrFailed;

    /* Whether amDone() has been called. */
    bool done;

    
    Goal(Worker & worker) : worker(worker)
    {
        done = false;
        nrFailed = 0;
    }

    virtual ~Goal()
    {
        printMsg(lvlVomit, "goal destroyed");
    }

public:
    virtual void work() = 0;

    virtual string name() = 0;

    void addWaitee(GoalPtr waitee);

    virtual void waiteeDone(GoalPtr waitee, bool success);

    virtual void writeLog(int fd, const unsigned char * buf, size_t count)
    {
        abort();
    }

    void trace(const format & f);
    
protected:
    void amDone(bool success = true);
};


/* A mapping used to remember for each child process to what goal it
   belongs, and a file descriptor for receiving log data. */
struct Child
{
    WeakGoalPtr goal;
    int fdOutput;
    bool inBuildSlot;
};

typedef map<pid_t, Child> Children;


/* The worker class. */
class Worker
{
private:

    /* Note: the worker should only have strong pointers to the
       top-level goals. */

    /* The top-level goals of the worker. */
    Goals topGoals;

    /* Goals that are ready to do some work. */
    WeakGoals awake;

    /* Goals waiting for a build slot. */
    WeakGoals wantingToBuild;

    /* Child processes currently running. */
    Children children;

    /* Number of build slots occupied.  Not all child processes
       (namely build hooks) count as occupied build slots. */
    unsigned int nrChildren;

    /* Maps used to prevent multiple instantiations of a goal for the
       same derivation / path. */
    WeakGoalMap derivationGoals;
    WeakGoalMap substitutionGoals;

public:

    Worker();
    ~Worker();

    /* Make a goal (with caching). */
    GoalPtr makeDerivationGoal(const Path & drvPath);
    GoalPtr makeSubstitutionGoal(const Path & storePath);

    /* Remove a dead goal. */
    void removeGoal(GoalPtr goal);

    /* Wake up a goal (i.e., there is something for it to do). */
    void wakeUp(GoalPtr goal);

    /* Can we start another child process? */
    bool canBuildMore();

    /* Registers / unregisters a running child process. */
    void childStarted(GoalPtr goal, pid_t pid, int fdOutput,
        bool inBuildSlot);
    void childTerminated(pid_t pid, bool wakeSleepers = true);

    /* Add a goal to the set of goals waiting for a build slot. */
    void waitForBuildSlot(GoalPtr goal, bool reallyWait = false);
    
    /* Loop until the specified top-level goal has finished.  Returns
       true if it has finished succesfully. */
    bool run(const Goals & topGoals);

    /* Wait for input to become available. */
    void waitForInput();
};


class SubstError : public Error
{
public:
    SubstError(const format & f) : Error(f) { };
};


class BuildError : public Error
{
public:
    BuildError(const format & f) : Error(f) { };
};



//////////////////////////////////////////////////////////////////////


void Goal::addWaitee(GoalPtr waitee)
{
    waitees.insert(waitee);
    waitee->waiters.insert(shared_from_this());
}


void Goal::waiteeDone(GoalPtr waitee, bool success)
{
    assert(waitees.find(waitee) != waitees.end());
    waitees.erase(waitee);
    
    if (!success) ++nrFailed;
    
    if (waitees.empty() || (!success && !keepGoing)) {

        /* If we failed and keepGoing is not set, we remove all
           remaining waitees. */
        for (Goals::iterator i = waitees.begin(); i != waitees.end(); ++i) {
            GoalPtr goal = *i;
            WeakGoals waiters2;
            for (WeakGoals::iterator j = goal->waiters.begin();
                 j != goal->waiters.end(); ++j)
                if (j->lock() != shared_from_this())
                    waiters2.insert(*j);
            goal->waiters = waiters2;
        }
        waitees.clear();

        worker.wakeUp(shared_from_this());
    }
}


void Goal::amDone(bool success)
{
    trace("done");
    assert(!done);
    done = true;
    for (WeakGoals::iterator i = waiters.begin(); i != waiters.end(); ++i) {
        GoalPtr goal = i->lock();
        if (goal) goal->waiteeDone(shared_from_this(), success);
    }
    waiters.clear();
    worker.removeGoal(shared_from_this());
}


void Goal::trace(const format & f)
{
    debug(format("%1%: %2%") % name() % f);
}



//////////////////////////////////////////////////////////////////////


/* Common initialisation performed in child processes. */
void commonChildInit(Pipe & logPipe)
{
    /* Put the child in a separate process group so that it doesn't
       receive terminal signals. */
    if (setpgid(0, 0) == -1)
        throw SysError(format("setting process group"));

    /* Dup the write side of the logger pipe into stderr. */
    if (dup2(logPipe.writeSide, STDERR_FILENO) == -1)
        throw SysError("cannot pipe standard error into log file");
    logPipe.readSide.close();
            
    /* Dup stderr to stdin. */
    if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1)
        throw SysError("cannot dup stderr into stdout");

    /* Reroute stdin to /dev/null. */
    int fdDevNull = open(pathNullDevice.c_str(), O_RDWR);
    if (fdDevNull == -1)
        throw SysError(format("cannot open `%1%'") % pathNullDevice);
    if (dup2(fdDevNull, STDIN_FILENO) == -1)
        throw SysError("cannot dup null device into stdin");
}


/* Convert a string list to an array of char pointers.  Careful: the
   string list should outlive the array. */
const char * * strings2CharPtrs(const Strings & ss)
{
    const char * * arr = new const char * [ss.size() + 1];
    const char * * p = arr;
    for (Strings::const_iterator i = ss.begin(); i != ss.end(); ++i)
        *p++ = i->c_str();
    *p = 0;
    return arr;
}



//////////////////////////////////////////////////////////////////////


class DerivationGoal : public Goal
{
private:
    /* The path of the derivation. */
    Path drvPath;

    /* The derivation stored at drvPath. */
    Derivation drv;
    
    /* The remainder is state held during the build. */

    /* Locks on the output paths. */
    PathLocks outputLocks;

    /* All input paths (that is, the union of FS closures of the
       immediate input paths). */
    PathSet inputPaths; 

    /* Referenceable paths (i.e., input and output paths). */
    PathSet allPaths;

    /* The process ID of the builder. */
    Pid pid;

    /* The temporary directory. */
    Path tmpDir;

    /* File descriptor for the log file. */
    AutoCloseFD fdLogFile;

    /* Pipe for the builder's standard output/error. */
    Pipe logPipe;

    /* Pipes for talking to the build hook (if any). */
    Pipe toHook;
    Pipe fromHook;

    typedef void (DerivationGoal::*GoalState)();
    GoalState state;
    
public:
    DerivationGoal(const Path & drvPath, Worker & worker);
    ~DerivationGoal();

    void work();

private:
    /* The states. */
    void init();
    void haveStoreExpr();
    void outputsSubstituted();
    void inputsRealised();
    void tryToBuild();
    void buildDone();

    /* Is the build hook willing to perform the build? */
    typedef enum {rpAccept, rpDecline, rpPostpone, rpDone} HookReply;
    HookReply tryBuildHook();

    /* Synchronously wait for a build hook to finish. */
    void terminateBuildHook();

    /* Acquires locks on the output paths and gathers information
       about the build (e.g., the input closures).  During this
       process its possible that we find out that the build is
       unnecessary, in which case we return false (this is not an
       error condition!). */
    bool prepareBuild();

    /* Start building a derivation. */
    void startBuilder();

    /* Must be called after the output paths have become valid (either
       due to a successful build or hook, or because they already
       were). */
    void computeClosure();

    /* Open a log file and a pipe to it. */
    void openLogFile();

    /* Common initialisation to be performed in child processes (i.e.,
       both in builders and in build hooks. */
    void initChild();
    
    /* Delete the temporary directory, if we have one. */
    void deleteTmpDir(bool force);

    /* Callback used by the worker to write to the log. */
    void writeLog(int fd, const unsigned char * buf, size_t count);

    /* Return the set of (in)valid paths. */
    PathSet checkPathValidity(bool returnValid);

    string name();
};


DerivationGoal::DerivationGoal(const Path & drvPath, Worker & worker)
    : Goal(worker)
{
    this->drvPath = drvPath;
    state = &DerivationGoal::init;
}


DerivationGoal::~DerivationGoal()
{
    if (pid != -1) worker.childTerminated(pid);
    
    /* Careful: we should never ever throw an exception from a
       destructor. */
    try {
        deleteTmpDir(false);
    } catch (Error & e) {
        printMsg(lvlError, format("error (ignored): %1%") % e.msg());
    }
}


void DerivationGoal::work()
{
    (this->*state)();
}


void DerivationGoal::init()
{
    trace("init");

    /* The first thing to do is to make sure that the derivation
       exists.  If it doesn't, it may be created through a
       substitute. */
    addWaitee(worker.makeSubstitutionGoal(drvPath));

    state = &DerivationGoal::haveStoreExpr;
}


void DerivationGoal::haveStoreExpr()
{
    trace("loading derivation");

    if (nrFailed != 0) {
        printMsg(lvlError,
            format("cannot build missing derivation `%1%'")
            % drvPath);
        amDone(false);
        return;
    }

    assert(isValidPath(drvPath));

    /* Get the derivation. */
    drv = derivationFromPath(drvPath);

    /* Check what outputs paths are not already valid. */
    PathSet invalidOutputs = checkPathValidity(false);

    /* If they are all valid, then we're done. */
    if (invalidOutputs.size() == 0) {
        amDone(true);
        return;
    }

    /* We are first going to try to create the invalid output paths
       through substitutes.  If that doesn't work, we'll build
       them. */
    for (PathSet::iterator i = invalidOutputs.begin();
         i != invalidOutputs.end(); ++i)
        /* Don't bother creating a substitution goal if there are no
           substitutes. */
        if (querySubstitutes(*i).size() > 0)
            addWaitee(worker.makeSubstitutionGoal(*i));

    if (waitees.empty()) /* to prevent hang (no wake-up event) */
        outputsSubstituted();
    else
        state = &DerivationGoal::outputsSubstituted;
}


void DerivationGoal::outputsSubstituted()
{
    trace("all outputs substituted (maybe)");

    if (nrFailed > 0 && !tryFallback)
        throw Error(format("some substitutes for the outputs of derivation `%1%' failed; try `--fallback'") % drvPath);

    nrFailed = 0;

    if (checkPathValidity(false).size() == 0) {
        amDone(true);
        return;
    }

    /* Otherwise, at least one of the output paths could not be
       produced using a substitute.  So we have to build instead. */

    /* The inputs must be built before we can build this goal. */
    /* !!! but if possible, only install the paths that we need */
    for (DerivationInputs::iterator i = drv.inputDrvs.begin();
         i != drv.inputDrvs.end(); ++i)
        addWaitee(worker.makeDerivationGoal(i->first));

    for (PathSet::iterator i = drv.inputSrcs.begin();
         i != drv.inputSrcs.end(); ++i)
        addWaitee(worker.makeSubstitutionGoal(*i));

    state = &DerivationGoal::inputsRealised;
}


void DerivationGoal::inputsRealised()
{
    trace("all inputs realised");

    if (nrFailed != 0) {
        printMsg(lvlError,
            format("cannot build derivation `%1%': "
                "%2% inputs could not be realised")
            % drvPath % nrFailed);
        amDone(false);
        return;
    }

    /* Okay, try to build.  Note that here we don't wait for a build
       slot to become available, since we don't need one if there is a
       build hook. */
    state = &DerivationGoal::tryToBuild;
    worker.wakeUp(shared_from_this());
}


void DerivationGoal::tryToBuild()
{
    trace("trying to build");

    try {

        /* Is the build hook willing to accept this job? */
        switch (tryBuildHook()) {
            case rpAccept:
                /* Yes, it has started doing so.  Wait until we get
                   EOF from the hook. */
                state = &DerivationGoal::buildDone;
                return;
            case rpPostpone:
                /* Not now; wait until at least one child finishes. */
                worker.waitForBuildSlot(shared_from_this(), true);
                return;
            case rpDecline:
                /* We should do it ourselves. */
                break;
            case rpDone:
                /* Somebody else did it. */
                amDone();
                return;
        }

        /* Make sure that we are allowed to start a build. */
        if (!worker.canBuildMore()) {
            worker.waitForBuildSlot(shared_from_this());
            return;
        }

        /* Acquire locks and such.  If we then see that the build has
           been done by somebody else, we're done. */
        if (!prepareBuild()) {
            amDone();
            return;
        }

        /* Okay, we have to build. */
        startBuilder();

    } catch (BuildError & e) {
        printMsg(lvlError, e.msg());
        amDone(false);
        return;
    }

    /* This state will be reached when we get EOF on the child's
       log pipe. */
    state = &DerivationGoal::buildDone;
}


void DerivationGoal::buildDone()
{
    trace("build done");

    /* Since we got an EOF on the logger pipe, the builder is presumed
       to have terminated.  In fact, the builder could also have
       simply have closed its end of the pipe --- just don't do that
       :-) */
    /* !!! this could block! */
    pid_t savedPid = pid;
    int status = pid.wait(true);

    /* So the child is gone now. */
    worker.childTerminated(savedPid);

    /* Close the read side of the logger pipe. */
    logPipe.readSide.close();

    /* Close the log file. */
    fdLogFile.close();

    debug(format("builder process for `%1%' finished") % drvPath);

    /* Check the exit status. */
    if (!statusOk(status)) {
        deleteTmpDir(false);
        printMsg(lvlError, format("builder for `%1%' %2%")
            % drvPath % statusToString(status));
        amDone(false);
        return;
    }
    
    deleteTmpDir(true);

    /* Compute the FS closure of the outputs and register them as
       being valid. */
    try {
        computeClosure();
    } catch (BuildError & e) {
        printMsg(lvlError, e.msg());
        amDone(false);
        return;
    }

    amDone();
}


static string readLine(int fd)
{
    string s;
    while (1) {
        char ch;
        ssize_t rd = read(fd, &ch, 1);
        if (rd == -1) {
            if (errno != EINTR)
                throw SysError("reading a line");
        } else if (rd == 0)
            throw Error("unexpected EOF reading a line");
        else {
            if (ch == '\n') return s;
            s += ch;
        }
    }
}


static void writeLine(int fd, string s)
{
    s += '\n';
    writeFull(fd, (const unsigned char *) s.c_str(), s.size());
}


/* !!! ugly hack */
static void drain(int fd)
{
    unsigned char buffer[1024];
    while (1) {
        ssize_t rd = read(fd, buffer, sizeof buffer);
        if (rd == -1) {
            if (errno != EINTR)
                throw SysError("draining");
        } else if (rd == 0) break;
        else writeFull(STDERR_FILENO, buffer, rd);
    }
}


PathSet outputPaths(const DerivationOutputs & outputs)
{
    PathSet paths;
    for (DerivationOutputs::const_iterator i = outputs.begin();
         i != outputs.end(); ++i)
        paths.insert(i->second.path);
    return paths;
}


string showPaths(const PathSet & paths)
{
    string s;
    for (PathSet::const_iterator i = paths.begin();
         i != paths.end(); ++i)
    {
        if (s.size() != 0) s += ", ";
        s += "`" + *i + "'";
    }
    return s;
}


DerivationGoal::HookReply DerivationGoal::tryBuildHook()
{
    Path buildHook = getEnv("NIX_BUILD_HOOK");
    if (buildHook == "") return rpDecline;
    buildHook = absPath(buildHook);

    /* Create a directory where we will store files used for
       communication between us and the build hook. */
    tmpDir = createTempDir();
    
    /* Create the log file and pipe. */
    openLogFile();

    /* Create the communication pipes. */
    toHook.create();
    fromHook.create();

    /* Fork the hook. */
    pid = fork();
    switch (pid) {
        
    case -1:
        throw SysError("unable to fork");

    case 0:
        try { /* child */

            initChild();

            execl(buildHook.c_str(), buildHook.c_str(),
                (worker.canBuildMore() ? (string) "1" : "0").c_str(),
                thisSystem.c_str(),
                drv.platform.c_str(),
                drvPath.c_str(), 0);
            
            throw SysError(format("executing `%1%'") % buildHook);
            
        } catch (exception & e) {
            cerr << format("build error: %1%\n") % e.what();
        }
        _exit(1);
    }
    
    /* parent */
    logPipe.writeSide.close();
    worker.childStarted(shared_from_this(),
        pid, logPipe.readSide, false);

    fromHook.writeSide.close();
    toHook.readSide.close();

    /* Read the first line of input, which should be a word indicating
       whether the hook wishes to perform the build.  !!! potential
       for deadlock here: we should also read from the child's logger
       pipe. */
    string reply;
    try {
        reply = readLine(fromHook.readSide);
    } catch (Error & e) {
        drain(logPipe.readSide);
        throw;
    }

    debug(format("hook reply is `%1%'") % reply);

    if (reply == "decline" || reply == "postpone") {
        /* Clean up the child.  !!! hacky / should verify */
        drain(logPipe.readSide);
        terminateBuildHook();
        return reply == "decline" ? rpDecline : rpPostpone;
    }

    else if (reply == "accept") {

        /* Acquire locks and such.  If we then see that the output
           paths are now valid, we're done. */
        if (!prepareBuild()) {
            /* Tell the hook to exit. */
            writeLine(toHook.writeSide, "cancel");
            terminateBuildHook();
            return rpDone;
        }

        printMsg(lvlInfo, format("running hook to build path(s) %1%")
            % showPaths(outputPaths(drv.outputs)));
        
        /* Write the information that the hook needs to perform the
           build, i.e., the set of input paths, the set of output
           paths, and the references (pointer graph) in the input
           paths. */
        
        Path inputListFN = tmpDir + "/inputs";
        Path outputListFN = tmpDir + "/outputs";
        Path referencesFN = tmpDir + "/references";

        string s;
        for (PathSet::iterator i = inputPaths.begin();
             i != inputPaths.end(); ++i)
            s += *i + "\n";
        for (DerivationInputs::iterator i = drv.inputDrvs.begin();
             i != drv.inputDrvs.end(); ++i)
            s += i->first + "\n";
        writeStringToFile(inputListFN, s);
        
        s = "";
        for (DerivationOutputs::iterator i = drv.outputs.begin();
             i != drv.outputs.end(); ++i)
            s += i->second.path + "\n";
        writeStringToFile(outputListFN, s);

        s = "";
        for (PathSet::iterator i = inputPaths.begin();
             i != inputPaths.end(); ++i)
        {
            s += *i;
            PathSet references;
            queryReferences(*i, references);
            for (PathSet::iterator j = references.begin();
                 j != references.end(); ++j)
            {
                s += " ";
                s += *j;
            }
            s += "\n";
        }
        writeStringToFile(referencesFN, s);

        /* Tell the hook to proceed. */ 
        writeLine(toHook.writeSide, "okay");

        return rpAccept;
    }

    else throw Error(format("bad hook reply `%1%'") % reply);
}


void DerivationGoal::terminateBuildHook()
{
    /* !!! drain stdout of hook */
    debug("terminating build hook");
    pid_t savedPid = pid;
    pid.wait(true);
    worker.childTerminated(savedPid, false);
    fromHook.readSide.close();
    toHook.writeSide.close();
    fdLogFile.close();
    logPipe.readSide.close();
    deleteTmpDir(true); /* get rid of the hook's temporary directory */
}


bool DerivationGoal::prepareBuild()
{
    /* Obtain locks on all output paths.  The locks are automatically
       released when we exit this function or Nix crashes. */
    /* !!! BUG: this could block, which is not allowed. */
    outputLocks.lockPaths(outputPaths(drv.outputs));

    /* Now check again whether the outputs are valid.  This is because
       another process may have started building in parallel.  After
       it has finished and released the locks, we can (and should)
       reuse its results.  (Strictly speaking the first check can be
       omitted, but that would be less efficient.)  Note that since we
       now hold the locks on the output paths, no other process can
       build this derivation, so no further checks are necessary. */
    PathSet validPaths = checkPathValidity(true);
    if (validPaths.size() == drv.outputs.size()) {
        debug(format("skipping build of derivation `%1%', someone beat us to it")
            % drvPath);
        outputLocks.setDeletion(true);
        return false;
    }

    if (validPaths.size() > 0) {
        /* !!! fix this; try to delete valid paths */
        throw Error(
            format("derivation `%1%' is blocked by its output paths")
            % drvPath);
    }

    /* Gather information necessary for computing the closure and/or
       running the build hook. */
    
    /* The outputs are referenceable paths. */
    for (DerivationOutputs::iterator i = drv.outputs.begin();
         i != drv.outputs.end(); ++i)
    {
        debug(format("building path `%1%'") % i->second.path);
        allPaths.insert(i->second.path);
    }

    /* Determine the full set of input paths. */

    /* First, the input derivations. */
    for (DerivationInputs::iterator i = drv.inputDrvs.begin();
         i != drv.inputDrvs.end(); ++i)
    {
        /* Add the relevant output closures of the input derivation
           `*i' as input paths.  Only add the closures of output paths
           that are specified as inputs. */
        /* !!! is `*i' present? */
        assert(isValidPath(i->first));
        Derivation inDrv = derivationFromPath(i->first);
        for (StringSet::iterator j = i->second.begin();
             j != i->second.end(); ++j)
            if (inDrv.outputs.find(*j) != inDrv.outputs.end())
                computeFSClosure(inDrv.outputs[*j].path, inputPaths);
            else
                throw Error(
                    format("derivation `%1%' requires non-existent output `%2%' from input derivation `%3%'")
                    % drvPath % *j % i->first);
    }

    debug(format("added input paths %1%") % showPaths(inputPaths));

    allPaths.insert(inputPaths.begin(), inputPaths.end());

    /* Second, the input sources. */
    for (PathSet::iterator i = drv.inputSrcs.begin();
         i != drv.inputSrcs.end(); ++i)
        computeFSClosure(*i, inputPaths);

    return true;
}


void DerivationGoal::startBuilder()
{
    startNest(nest, lvlInfo,
        format("building path(s) %1%") % showPaths(outputPaths(drv.outputs)))
    
    /* Right platform? */
    if (drv.platform != thisSystem)
        throw BuildError(
            format("a `%1%' is required to build `%3%', but I am a `%2%'")
            % drv.platform % thisSystem % drvPath);

    /* If any of the outputs already exist but are not registered,
       delete them. */
    for (DerivationOutputs::iterator i = drv.outputs.begin(); 
         i != drv.outputs.end(); ++i)
    {
        Path path = i->second.path;
        if (isValidPath(path))
            throw Error(format("obstructed build: path `%1%' exists") % path);
        if (pathExists(path)) {
            debug(format("removing unregistered path `%1%'") % path);
            deletePath(path);
        }
    }

    /* Construct the environment passed to the builder. */
    typedef map<string, string> Environment;
    Environment env; 
    
    /* Most shells initialise PATH to some default (/bin:/usr/bin:...) when
       PATH is not set.  We don't want this, so we fill it in with some dummy
       value. */
    env["PATH"] = "/path-not-set";

    /* Set HOME to a non-existing path to prevent certain programs from using
       /etc/passwd (or NIS, or whatever) to locate the home directory (for
       example, wget looks for ~/.wgetrc).  I.e., these tools use /etc/passwd
       if HOME is not set, but they will just assume that the settings file
       they are looking for does not exist if HOME is set but points to some
       non-existing path. */
    env["HOME"] = "/homeless-shelter";

    /* Tell the builder where the Nix store is.  Usually they
       shouldn't care, but this is useful for purity checking (e.g.,
       the compiler or linker might only want to accept paths to files
       in the store or in the build directory). */
    env["NIX_STORE"] = nixStore;

    /* Add all bindings specified in the derivation. */
    for (StringPairs::iterator i = drv.env.begin();
         i != drv.env.end(); ++i)
        env[i->first] = i->second;

    /* Create a temporary directory where the build will take
       place. */
    tmpDir = createTempDir();

    /* For convenience, set an environment pointing to the top build
       directory. */
    env["NIX_BUILD_TOP"] = tmpDir;

    /* Also set TMPDIR and variants to point to this directory. */
    env["TMPDIR"] = env["TEMPDIR"] = env["TMP"] = env["TEMP"] = tmpDir;

    /* Run the builder. */
    printMsg(lvlChatty, format("executing builder `%1%'") %
        drv.builder);

    /* Create the log file and pipe. */
    openLogFile();
    
    /* Fork a child to build the package.  Note that while we
       currently use forks to run and wait for the children, it
       shouldn't be hard to use threads for this on systems where
       fork() is unavailable or inefficient. */
    pid = fork();
    switch (pid) {

    case -1:
        throw SysError("unable to fork");

    case 0:

        /* Warning: in the child we should absolutely not make any
           Berkeley DB calls! */

        try { /* child */

            initChild();

            /* Fill in the arguments. */
            Strings args(drv.args);
            args.push_front(baseNameOf(drv.builder));
            const char * * argArr = strings2CharPtrs(args);

            /* Fill in the environment. */
            Strings envStrs;
            for (Environment::const_iterator i = env.begin();
                 i != env.end(); ++i)
                envStrs.push_back(i->first + "=" + i->second);
            const char * * envArr = strings2CharPtrs(envStrs);

            /* Execute the program.  This should not return. */
            execve(drv.builder.c_str(),
                (char * *) argArr, (char * *) envArr);

            throw SysError(format("executing `%1%'")
                % drv.builder);
            
        } catch (exception & e) {
            cerr << format("build error: %1%\n") % e.what();
        }
        _exit(1);
    }

    /* parent */
    pid.setSeparatePG(true);
    logPipe.writeSide.close();
    worker.childStarted(shared_from_this(),
        pid, logPipe.readSide, true);
}


void DerivationGoal::computeClosure()
{
    startNest(nest, lvlTalkative,
        format("determining closure for `%1%'") % drvPath);

    map<Path, PathSet> allReferences;
    map<Path, Hash> contentHashes;
    
    /* Check whether the output paths were created, and grep each
       output path to determine what other paths it references.  Also make all
       output paths read-only. */
    for (DerivationOutputs::iterator i = drv.outputs.begin(); 
         i != drv.outputs.end(); ++i)
    {
        Path path = i->second.path;
        if (!pathExists(path)) {
            throw BuildError(
                format("builder for `%1%' failed to produce output path `%2%'")
                % drvPath % path);
        }

        /* Check that fixed-output derivations produced the right
           outputs (i.e., the content hash should match the specified
           hash). */ 
        if (i->second.hash != "") {
            HashType ht = parseHashType(i->second.hashAlgo);
            if (ht == htUnknown)
                throw Error(format("unknown hash algorithm `%1%'") % i->second.hashAlgo);
            Hash h = parseHash(ht, i->second.hash);
            Hash h2 = hashFile(ht, path);
            if (h != h2)
                throw Error(
                    format("output path `%1% should have %2% hash `%3%', instead has `%4%'")
                    % path % i->second.hashAlgo % printHash(h) % printHash(h2));

            /* Also, the output path should be a regular file withouth
               execute permission. */
            struct stat st;
            if (lstat(path.c_str(), &st))
                throw SysError(format("getting attributes of path `%1%'") % path);
            if (!S_ISREG(st.st_mode) || (st.st_mode & S_IXUSR) != 0)
                throw Error(
                    format("output path `%1% should be a non-executable regular file")
                    % path);
        }

	canonicalisePathMetaData(path);

	/* For this output path, find the references to other paths contained
	   in it. */
        PathSet references;
        if (!pathExists(path + "/nix-support/no-scan")) {
            startNest(nest2, lvlChatty,
                format("scanning for store references in `%1%'") % path);
            Paths references2;
            references2 = filterReferences(path, 
                Paths(allPaths.begin(), allPaths.end()));
            references = PathSet(references2.begin(), references2.end());

            /* For debugging, print out the referenced and
               unreferenced paths. */
            for (PathSet::iterator i = inputPaths.begin();
                 i != inputPaths.end(); ++i)
            {
                PathSet::iterator j = references.find(*i);
                if (j == references.end())
                    debug(format("unreferenced input: `%1%'") % *i);
                else
                    debug(format("referenced input: `%1%'") % *i);
            }
            
            nest2.close();
        }

        allReferences[path] = references;

        /* Hash the contents of the path.  The hash is stored in the
           database so that we can verify later on whether nobody has
           messed with the store.  !!! inefficient: it would be nice
           if we could combine this with filterReferences(). */
        contentHashes[path] = hashPath(htSHA256, path);
    }

    /* Register each output path as valid, and register the sets of
       paths referenced by each of them.  This is wrapped in one
       database transaction to ensure that if we crash, either
       everything is registered or nothing is.  This is for
       recoverability: unregistered paths in the store can be deleted
       arbitrarily, while registered paths can only be deleted by
       running the garbage collector.

       The reason that we do the transaction here and not on the fly
       while we are scanning (above) is so that we don't hold database
       locks for too long. */
    Transaction txn;
    createStoreTransaction(txn);
    for (DerivationOutputs::iterator i = drv.outputs.begin(); 
         i != drv.outputs.end(); ++i)
    {
        registerValidPath(txn, i->second.path,
            contentHashes[i->second.path]);
        setReferences(txn, i->second.path,
            allReferences[i->second.path]);
    }
    txn.commit();

    /* It is now safe to delete the lock files, since all future
       lockers will see that the output paths are valid; they will not
       create new lock files with the same names as the old (unlinked)
       lock files. */
    outputLocks.setDeletion(true);
}


void DerivationGoal::openLogFile()
{
    /* Create a log file. */
    Path logFileName = nixLogDir + "/" + baseNameOf(drvPath);
    fdLogFile = open(logFileName.c_str(),
        O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fdLogFile == -1)
        throw SysError(format("creating log file `%1%'") % logFileName);

    /* Create a pipe to get the output of the child. */
    logPipe.create();
}


void DerivationGoal::initChild()
{
    commonChildInit(logPipe);
    
    if (chdir(tmpDir.c_str()) == -1)
        throw SysError(format("changing into `%1%'") % tmpDir);

    /* When running a hook, dup the communication pipes. */
    bool inHook = fromHook.writeSide.isOpen();
    if (inHook) {
        fromHook.readSide.close();
        if (dup2(fromHook.writeSide, 3) == -1)
            throw SysError("dupping from-hook write side");

        toHook.writeSide.close();
        if (dup2(toHook.readSide, 4) == -1)
            throw SysError("dupping to-hook read side");
    }

    /* Close all other file descriptors. */
    int maxFD = 0;
    maxFD = sysconf(_SC_OPEN_MAX);
    for (int fd = 0; fd < maxFD; ++fd)
        if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO
            && (!inHook || (fd != 3 && fd != 4)))
            close(fd); /* ignore result */
}


void DerivationGoal::deleteTmpDir(bool force)
{
    if (tmpDir != "") {
        if (keepFailed && !force)
	    printMsg(lvlError, 
		format("builder for `%1%' failed; keeping build directory `%2%'")
                % drvPath % tmpDir);
        else
            deletePath(tmpDir);
        tmpDir = "";
    }
}


void DerivationGoal::writeLog(int fd,
    const unsigned char * buf, size_t count)
{
    assert(fd == logPipe.readSide);
    writeFull(fdLogFile, buf, count);
}


PathSet DerivationGoal::checkPathValidity(bool returnValid)
{
    PathSet result;
    for (DerivationOutputs::iterator i = drv.outputs.begin();
         i != drv.outputs.end(); ++i)
        if (isValidPath(i->second.path)) {
            if (returnValid) result.insert(i->second.path);
        } else {
            if (!returnValid) result.insert(i->second.path);
        }
    return result;
}


string DerivationGoal::name()
{
    return (format("building of `%1%'") % drvPath).str();
}



//////////////////////////////////////////////////////////////////////


class SubstitutionGoal : public Goal
{
private:
    /* The store path that should be realised through a substitute. */
    Path storePath;

    /* The remaining substitutes for this path. */
    Substitutes subs;

    /* The current substitute. */
    Substitute sub;

    /* Outgoing references for this path. */
    PathSet references;

    /* Pipe for the substitute's standard output/error. */
    Pipe logPipe;

    /* The process ID of the builder. */
    Pid pid;

    /* Lock on the store path. */
    shared_ptr<PathLocks> outputLock;
    
    typedef void (SubstitutionGoal::*GoalState)();
    GoalState state;

public:
    SubstitutionGoal(const Path & storePath, Worker & worker);
    ~SubstitutionGoal();

    void work();

    /* The states. */
    void init();
    void referencesValid();
    void tryNext();
    void tryToRun();
    void finished();

    /* Callback used by the worker to write to the log. */
    void writeLog(int fd, const unsigned char * buf, size_t count);

    string name();
};


SubstitutionGoal::SubstitutionGoal(const Path & storePath, Worker & worker)
    : Goal(worker)
{
    this->storePath = storePath;
    state = &SubstitutionGoal::init;
}


SubstitutionGoal::~SubstitutionGoal()
{
    if (pid != -1) worker.childTerminated(pid);
}


void SubstitutionGoal::work()
{
    (this->*state)();
}


void SubstitutionGoal::init()
{
    trace("init");

    /* If the path already exists we're done. */
    if (isValidPath(storePath)) {
        amDone();
        return;
    }

    /* Read the substitutes. */
    subs = querySubstitutes(storePath);

    /* To maintain the closure invairant, we first have to realise the
       paths referenced by this one. */
    queryReferences(storePath, references);

    for (PathSet::iterator i = references.begin();
         i != references.end(); ++i)
        addWaitee(worker.makeSubstitutionGoal(*i));

    if (waitees.empty()) /* to prevent hang (no wake-up event) */
        referencesValid();
    else
        state = &SubstitutionGoal::referencesValid;
}


void SubstitutionGoal::referencesValid()
{
    trace("all referenced realised");

    if (nrFailed > 0)
        throw Error(format("some references of path `%1%' could not be realised") % storePath);

    for (PathSet::iterator i = references.begin();
         i != references.end(); ++i)
        assert(isValidPath(*i));
    
    tryNext();
}


void SubstitutionGoal::tryNext()
{
    trace("trying next substitute");

    if (subs.size() == 0) {
        /* None left.  Terminate this goal and let someone else deal
           with it. */
        printMsg(lvlError,
            format("path `%1%' is required, but it has no (remaining) substitutes")
            % storePath);
        amDone(false);
        return;
    }
    sub = subs.front();
    subs.pop_front();

    /* Wait until we can run the substitute program. */
    state = &SubstitutionGoal::tryToRun;
    worker.waitForBuildSlot(shared_from_this());
}


void SubstitutionGoal::tryToRun()
{
    trace("trying to run");

    /* Make sure that we are allowed to start a build. */
    if (!worker.canBuildMore()) {
        worker.waitForBuildSlot(shared_from_this());
        return;
    }

    /* Acquire a lock on the output path. */
    PathSet lockPath;
    lockPath.insert(storePath);
    outputLock = shared_ptr<PathLocks>(new PathLocks);
    outputLock->lockPaths(lockPath);

    /* Check again whether the path is invalid. */
    if (isValidPath(storePath)) {
        debug(format("store path `%1%' has become valid") % storePath);
        outputLock->setDeletion(true);
        amDone();
        return;
    }

    printMsg(lvlInfo,
        format("substituting path `%1%' using substituter `%2%'")
        % storePath % sub.program);
    
    logPipe.create();

    /* Remove the (stale) output path if it exists. */
    if (pathExists(storePath))
        deletePath(storePath);

    /* Fork the substitute program. */
    pid = fork();
    switch (pid) {
        
    case -1:
        throw SysError("unable to fork");

    case 0:
        try { /* child */

            logPipe.readSide.close();

            /* !!! close other handles */

            commonChildInit(logPipe);

            /* Fill in the arguments. */
            Strings args(sub.args);
            args.push_front(storePath);
            args.push_front(baseNameOf(sub.program));
            const char * * argArr = strings2CharPtrs(args);

            execv(sub.program.c_str(), (char * *) argArr);
            
            throw SysError(format("executing `%1%'") % sub.program);
            
        } catch (exception & e) {
            cerr << format("substitute error: %1%\n") % e.what();
        }
        _exit(1);
    }
    
    /* parent */
    pid.setSeparatePG(true);
    logPipe.writeSide.close();
    worker.childStarted(shared_from_this(),
        pid, logPipe.readSide, true);

    state = &SubstitutionGoal::finished;
}


void SubstitutionGoal::finished()
{
    trace("substitute finished");

    /* Since we got an EOF on the logger pipe, the substitute is
       presumed to have terminated.  */
    /* !!! this could block! */
    pid_t savedPid = pid;
    int status = pid.wait(true);

    /* So the child is gone now. */
    worker.childTerminated(savedPid);

    /* Close the read side of the logger pipe. */
    logPipe.readSide.close();

    debug(format("substitute for `%1%' finished") % storePath);

    /* Check the exit status and the build result. */
    try {
        
        if (!statusOk(status))
            throw SubstError(format("builder for `%1%' %2%")
                % storePath % statusToString(status));

        if (!pathExists(storePath))
            throw SubstError(
                format("substitute did not produce path `%1%'")
                % storePath);
        
    } catch (SubstError & e) {

        printMsg(lvlInfo,
            format("substitution of path `%1%' using substituter `%2%' failed: %3%")
            % storePath % sub.program % e.msg());
        
        /* Try the next substitute. */
        state = &SubstitutionGoal::tryNext;
        worker.wakeUp(shared_from_this());
        return;
    }

    canonicalisePathMetaData(storePath);

    Hash contentHash = hashPath(htSHA256, storePath);

    Transaction txn;
    createStoreTransaction(txn);
    registerValidPath(txn, storePath, contentHash);
    txn.commit();

    outputLock->setDeletion(true);
    
    printMsg(lvlChatty,
        format("substitution of path `%1%' succeeded") % storePath);

    amDone();
}


void SubstitutionGoal::writeLog(int fd,
    const unsigned char * buf, size_t count)
{
    assert(fd == logPipe.readSide);
    /* Don't write substitution output to a log file for now.  We
       probably should, though. */
}


string SubstitutionGoal::name()
{
    return (format("substitution of `%1%'") % storePath).str();
}



//////////////////////////////////////////////////////////////////////


/* A fake goal used to receive notification of success or failure of
   other goals. */
class PseudoGoal : public Goal
{
private:
    bool success;
    
public:
    PseudoGoal(Worker & worker) : Goal(worker)
    {
        success = true;
    }

    void work() 
    {
        abort();
    }

    void waiteeDone(GoalPtr waitee, bool success)
    {
        if (!success) this->success = false;
    }

    bool isOkay()
    {
        return success;
    }

    string name()
    {
        return "pseudo-goal";
    }
};



//////////////////////////////////////////////////////////////////////


static bool working = false;


Worker::Worker()
{
    /* Debugging: prevent recursive workers. */ 
    if (working) abort();
    working = true;
    nrChildren = 0;
}


Worker::~Worker()
{
    working = false;

    /* Explicitly get rid of all strong pointers now.  After this all
       goals that refer to this worker should be gone.  (Otherwise we
       are in trouble, since goals may call childTerminated() etc. in
       their destructors). */
    topGoals.clear();
}


template<class T>
static GoalPtr addGoal(const Path & path,
    Worker & worker, WeakGoalMap & goalMap)
{
    GoalPtr goal = goalMap[path].lock();
    if (!goal) {
        goal = GoalPtr(new T(path, worker));
        goalMap[path] = goal;
        worker.wakeUp(goal);
    }
    return goal;
}


GoalPtr Worker::makeDerivationGoal(const Path & nePath)
{
    return addGoal<DerivationGoal>(nePath, *this, derivationGoals);
}


GoalPtr Worker::makeSubstitutionGoal(const Path & storePath)
{
    return addGoal<SubstitutionGoal>(storePath, *this, substitutionGoals);
}


static void removeGoal(GoalPtr goal, WeakGoalMap & goalMap)
{
    /* !!! For now we just let dead goals accumulate.  We should
       probably periodically sweep the goalMap to remove dead
       goals. */
}


void Worker::removeGoal(GoalPtr goal)
{
    topGoals.erase(goal);
    ::removeGoal(goal, derivationGoals);
    ::removeGoal(goal, substitutionGoals);
}


void Worker::wakeUp(GoalPtr goal)
{
    goal->trace("woken up");
    awake.insert(goal);
}


bool Worker::canBuildMore()
{
    return nrChildren < maxBuildJobs;
}


void Worker::childStarted(GoalPtr goal,
    pid_t pid, int fdOutput, bool inBuildSlot)
{
    Child child;
    child.goal = goal;
    child.fdOutput = fdOutput;
    child.inBuildSlot = inBuildSlot;
    children[pid] = child;
    if (inBuildSlot) nrChildren++;
}


void Worker::childTerminated(pid_t pid, bool wakeSleepers)
{
    Children::iterator i = children.find(pid);
    assert(i != children.end());

    if (i->second.inBuildSlot) {
        assert(nrChildren > 0);
        nrChildren--;
    }

    children.erase(pid);

    if (wakeSleepers) {
        
        /* Wake up goals waiting for a build slot. */
        for (WeakGoals::iterator i = wantingToBuild.begin();
             i != wantingToBuild.end(); ++i)
        {
            GoalPtr goal = i->lock();
            if (goal) wakeUp(goal);
        }

        wantingToBuild.clear();
        
    }
}


void Worker::waitForBuildSlot(GoalPtr goal, bool reallyWait)
{
    debug("wait for build slot");
    if (reallyWait && children.size() == 0)
        throw Error("waiting for a build slot, yet there are no children - "
            "maybe the build hook gave an inappropriate `postpone' reply?");
    if (!reallyWait && canBuildMore())
        wakeUp(goal); /* we can do it right away */
    else
        wantingToBuild.insert(goal);
}


bool Worker::run(const Goals & _topGoals)
{
    /* Wrap the specified top-level goal in a pseudo-goal so that we
       can check whether it succeeded. */
    shared_ptr<PseudoGoal> pseudo(new PseudoGoal(*this));
    for (Goals::iterator i = _topGoals.begin();
         i != _topGoals.end(); ++i)
    {
        assert(*i);
        pseudo->addWaitee(*i);
        topGoals.insert(*i);
    }
    
    startNest(nest, lvlDebug, format("entered goal loop"));

    while (1) {

        checkInterrupt();

        /* Call every wake goal. */
        while (!awake.empty()) {
            WeakGoals awake2(awake);
            awake.clear();
            for (WeakGoals::iterator i = awake2.begin(); i != awake2.end(); ++i) {
                checkInterrupt();
                GoalPtr goal = i->lock();
                if (goal) goal->work();
            }
        }

        if (topGoals.empty()) break;

        /* !!! not when we're polling */
        assert(!children.empty());
        
        /* Wait for input. */
        waitForInput();
    }

    /* If --keep-going is not set, it's possible that the main goal
       exited while some of its subgoals were still active.  But if
       --keep-going *is* set, then they must all be finished now. */
    assert(!keepGoing || awake.empty());
    assert(!keepGoing || wantingToBuild.empty());
    assert(!keepGoing || children.empty());

    return pseudo->isOkay();
}


void Worker::waitForInput()
{
    printMsg(lvlVomit, "waiting for children");

    /* Process log output from the children.  We also use this to
       detect child termination: if we get EOF on the logger pipe of a
       build, we assume that the builder has terminated. */

    /* Use select() to wait for the input side of any logger pipe to
       become `available'.  Note that `available' (i.e., non-blocking)
       includes EOF. */
    fd_set fds;
    FD_ZERO(&fds);
    int fdMax = 0;
    for (Children::iterator i = children.begin();
         i != children.end(); ++i)
    {
        int fd = i->second.fdOutput;
        FD_SET(fd, &fds);
        if (fd >= fdMax) fdMax = fd + 1;
    }

    if (select(fdMax, &fds, 0, 0, 0) == -1) {
        if (errno == EINTR) return;
        throw SysError("waiting for input");
    }

    /* Process all available file descriptors. */
    for (Children::iterator i = children.begin();
         i != children.end(); ++i)
    {
        checkInterrupt();
        GoalPtr goal = i->second.goal.lock();
        assert(goal);
        int fd = i->second.fdOutput;
        if (FD_ISSET(fd, &fds)) {
            unsigned char buffer[4096];
            ssize_t rd = read(fd, buffer, sizeof(buffer));
            if (rd == -1) {
                if (errno != EINTR)
                    throw SysError(format("reading from %1%")
                        % goal->name());
            } else if (rd == 0) {
                debug(format("%1%: got EOF") % goal->name());
                wakeUp(goal);
            } else {
                printMsg(lvlVomit, format("%1%: read %2% bytes")
                    % goal->name() % rd);
                goal->writeLog(fd, buffer, (size_t) rd);
                if (verbosity >= buildVerbosity)
                    writeFull(STDERR_FILENO, buffer, rd);
            }
        }
    }
}


//////////////////////////////////////////////////////////////////////


void buildDerivations(const PathSet & drvPaths)
{
    startNest(nest, lvlDebug,
        format("building %1%") % showPaths(drvPaths));

    Worker worker;

    Goals goals;
    for (PathSet::const_iterator i = drvPaths.begin();
         i != drvPaths.end(); ++i)
        goals.insert(worker.makeDerivationGoal(*i));
    
    if (!worker.run(goals))
        throw Error(format("build failed"));
}


void ensurePath(const Path & path)
{
    /* If the path is already valid, we're done. */
    if (isValidPath(path)) return;

    Worker worker;
    Goals goals;
    goals.insert(worker.makeSubstitutionGoal(path));
    if (!worker.run(goals))
        throw Error(format("path `%1%' does not exist and cannot be created") % path);
}