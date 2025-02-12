/* Copyright (C) 2009-2020 J.F.Dockes
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "autoconfig.h"

#include "fsindexer.h"

#include <stdio.h>
#include <errno.h>
#include <cstring>

#include <iostream>
#include <list>
#include <map>
#include <algorithm>

#include "cstr.h"
#include "pathut.h"
#include "rclutil.h"
#include "conftree.h"
#include "rclconfig.h"
#include "fstreewalk.h"
#include "rcldb.h"
#include "readfile.h"
#include "indexer.h"
#include "transcode.h"
#include "log.h"
#include "internfile.h"
#include "smallut.h"
#include "chrono.h"
#include "wipedir.h"
#include "fileudi.h"
#include "cancelcheck.h"
#include "rclinit.h"
#include "extrameta.h"
#include "idxdiags.h"
#include "fsfetcher.h"
#if defined(HAVE_POSIX_FADVISE)
#include <unistd.h>
#include <fcntl.h>
#endif

using namespace std;

#ifdef IDX_THREADS
class DbUpdTask {
public:
    // Take some care to avoid sharing string data (if string impl is cow)
    DbUpdTask(const string& u, const string& p, const Rcl::Doc& d)
        : udi(u.begin(), u.end()), parent_udi(p.begin(), p.end())
        {
            d.copyto(&doc);
        }
    string udi;
    string parent_udi;
    Rcl::Doc doc;
};
extern void *FsIndexerDbUpdWorker(void*);

class InternfileTask {
public:
    // Take some care to avoid sharing string data (if string impl is cow)
    InternfileTask(const std::string &f, const struct PathStat& i_stp,
                   map<string,string> lfields)
        : fn(f.begin(), f.end()), statbuf(i_stp)
        {
            map_ss_cp_noshr(lfields, &localfields);
        }
    string fn;
    struct PathStat statbuf;
    map<string,string> localfields;
};
extern void *FsIndexerInternfileWorker(void*);
#endif // IDX_THREADS

// Thread safe variation of the "missing helpers" storage. Only the
// addMissing method needs protection, the rest are called from the
// main thread either before or after the exciting part
class FSIFIMissingStore : public FIMissingStore {
#ifdef IDX_THREADS
    std::mutex m_mutex;
#endif
public:
    virtual void addMissing(const string& prog, const string& mt)
        {
#ifdef IDX_THREADS
            std::unique_lock<std::mutex> locker(m_mutex);
#endif
            FIMissingStore::addMissing(prog, mt);
        }
};

FsIndexer::FsIndexer(RclConfig *cnf, Rcl::Db *db) 
    : m_config(cnf), m_db(db),
      m_missing(new FSIFIMissingStore), m_detectxattronly(false),
      m_noretryfailed(false)
#ifdef IDX_THREADS
    , m_iwqueue("Internfile", cnf->getThrConf(RclConfig::ThrIntern).first), 
      m_dwqueue("Split", cnf->getThrConf(RclConfig::ThrSplit).first)
#endif // IDX_THREADS
{
    LOGDEB1("FsIndexer::FsIndexer\n");
    m_havelocalfields = m_config->hasNameAnywhere("localfields");
    m_config->getConfParam("detectxattronly", &m_detectxattronly);
    
#ifdef IDX_THREADS
    m_stableconfig = new RclConfig(*m_config);
    m_haveInternQ = m_haveSplitQ = false;
    int internqlen = cnf->getThrConf(RclConfig::ThrIntern).first;
    int internthreads = cnf->getThrConf(RclConfig::ThrIntern).second;
    if (internqlen >= 0) {
        if (!m_iwqueue.start(internthreads, FsIndexerInternfileWorker, this)) {
            LOGERR("FsIndexer::FsIndexer: intern worker start failed\n");
            return;
        }
        m_haveInternQ = true;
    } 
    int splitqlen = cnf->getThrConf(RclConfig::ThrSplit).first;
    int splitthreads = cnf->getThrConf(RclConfig::ThrSplit).second;
    if (splitqlen >= 0) {
        if (!m_dwqueue.start(splitthreads, FsIndexerDbUpdWorker, this)) {
            LOGERR("FsIndexer::FsIndexer: split worker start failed\n");
            return;
        }
        m_haveSplitQ = true;
    }
    LOGDEB("FsIndexer: threads: haveIQ " << m_haveInternQ << " iql " <<
           internqlen << " iqts " << internthreads << " haveSQ " <<
           m_haveSplitQ << " sql " << splitqlen << " sqts " << splitthreads <<
           "\n");
#endif // IDX_THREADS
}

FsIndexer::~FsIndexer() 
{
    LOGDEB1("FsIndexer::~FsIndexer()\n");

#ifdef IDX_THREADS
    void *status;
    if (m_haveInternQ) {
        status = m_iwqueue.setTerminateAndWait();
        LOGDEB0("FsIndexer: internfile wrkr status: "<< status << " (1->ok)\n");
    }
    if (m_haveSplitQ) {
        status = m_dwqueue.setTerminateAndWait();
        LOGDEB0("FsIndexer: dbupd worker status: " << status << " (1->ok)\n");
    }
    delete m_stableconfig;
#endif // IDX_THREADS

    delete m_missing;
}

bool FsIndexer::init()
{
    if (m_tdl.empty()) {
        m_tdl = m_config->getTopdirs();
        if (m_tdl.empty()) {
            LOGERR("FsIndexers: no topdirs list defined\n");
            return false;
        }
    }
    return true;
}

// Recursively index each directory in the topdirs:
bool FsIndexer::index(int flags)
{
    bool quickshallow = (flags & ConfIndexer::IxFQuickShallow) != 0;
    m_noretryfailed = (flags & ConfIndexer::IxFNoRetryFailed) != 0;
    m_cleancache = (flags & ConfIndexer::IxFCleanCache) != 0;
    Chrono chron;
    if (!init())
        return false;

    statusUpdater()->setDbTotDocs(m_db->docCnt());

    m_walker.setSkippedPaths(m_config->getSkippedPaths());
    if (quickshallow) {
        m_walker.setOpts(m_walker.getOpts() | FsTreeWalker::FtwSkipDotFiles);
        m_walker.setMaxDepth(2);
    }

    bool walkok(true);
    for (const auto& topdir : m_tdl) {
        LOGDEB("FsIndexer::index: Indexing " << topdir << " into " <<
               getDbDir() << "\n");

        // If a topdirs member appears to be not here or not mounted
        // (empty), avoid deleting all the related index content by
        // marking the current docs as existing.
        if (path_empty(topdir)) {
            m_db->udiTreeMarkExisting(topdir);
            continue;
        }
        
        // Set the current directory in config so that subsequent
        // getConfParams() will get local values
        m_config->setKeyDir(topdir);

        // Adjust the "follow symlinks" option
        bool follow{false};
        int opts = m_walker.getOpts();
        if (m_config->getConfParam("followLinks", &follow) && follow) {
            opts |= FsTreeWalker::FtwFollow;
        } else {
            opts &= ~FsTreeWalker::FtwFollow;
        }           
        m_walker.setOpts(opts);

        int abslen;
        if (m_config->getConfParam("idxabsmlen", &abslen))
            m_db->setAbstractParams(abslen, -1, -1);

        // Walk the directory tree
        if (m_walker.walk(topdir, *this) != FsTreeWalker::FtwOk) {
            LOGERR("FsIndexer::index: error while indexing " << topdir <<
                   ": " << m_walker.getReason() << "\n");
            // DO NOT return: we need to flush the queues before the Db can be closed !
            walkok = false;
            break;
        }
        auto reason = m_walker.getReason();
        // Log walker events at a lower level
        if (!reason.empty()) {
            LOGINF("FsIndexer::index: walker events:\n" << reason << "\n");
        }
    }

    shutdownQueues(walkok);
    if (m_missing) {
        string missing;
        m_missing->getMissingDescription(missing);
        if (!missing.empty()) {
            LOGINFO("FsIndexer::index missing helper program(s):\n" << missing << "\n");
        }
        m_config->storeMissingHelperDesc(missing);
    }
    LOGINFO("fsindexer: status: " << walkok << " index time:  " << chron.millis() << " mS\n");
    return walkok;
}

void FsIndexer::shutdownQueues(bool ok)
{
#ifdef IDX_THREADS
    if (!ok) {
        // Error or more probably interrupt. Discard everything for fast shutdown
        if (m_haveInternQ)  {
            m_iwqueue.closeShop();
        }
        if (m_haveSplitQ) {
            m_dwqueue.closeShop();
        }
        m_db->closeQueue();
    }
    if (m_haveInternQ)  {
        m_iwqueue.waitIdle();
    }
    if (m_haveSplitQ) {
        m_dwqueue.waitIdle();
    }
    m_db->waitUpdIdle();
#endif // IDX_THREADS
}

static bool matchesSkipped(
    const vector<string>& tdl, FsTreeWalker& walker, const string& path)
{
    // Check path against topdirs and skippedPaths. We go up the
    // ancestors until we find either a topdirs or a skippedPaths
    // match. If topdirs is found first-> ok to index (it's possible
    // and useful to configure a topdir under a skippedPath in the
    // config). This matches what happens during the normal fs tree
    // walk.
    string canonpath = path_canon(path);

    string mpath = canonpath;
    string topdir;
    for (;;) { // Used to test not root here, but root may be in topdirs !

        for (const auto& tdlent : tdl) {
            // the topdirs members are already canonized.
            LOGDEB1("matchesSkipped: comparing ancestor [" << mpath <<
                    "] to topdir [" << tdlent << "]\n");
            if (mpath == tdlent) {
                topdir = tdlent;
                goto goodpath;
            }
        }

        if (walker.inSkippedPaths(mpath, false)) {
            LOGDEB("FsIndexer::indexFiles: skipping [" << path << "] (skpp)\n");
            return true;
        }

        if (path_isroot(mpath)) {
            break;
        }

        // Compute father
        string::size_type len = mpath.length();
        mpath = path_getfather(mpath);
        // getfather normally returns a path ending with /, canonic
        // paths don't (except for '/' itself).
        if (!path_isroot(mpath) && mpath[mpath.size()-1] == '/')
            mpath.erase(mpath.size()-1);
        // should not be necessary, but lets be prudent. If the
        // path did not shorten, something is seriously amiss
        // (could be an assert actually)
        if (mpath.length() >= len) {
            LOGERR("FsIndexer::indexFile: internal Error: path [" << mpath <<
                   "] did not shorten\n");
            return true;
        }

    }

    // We get there if neither topdirs nor skippedPaths tests matched
    LOGDEB("FsIndexer::indexFiles: skipping [" << path << "] (ntd)\n");
    return true;

goodpath:

    // Then check all path components up to the topdir against skippedNames
    mpath = canonpath;
    while (mpath.length() >= topdir.length() && mpath.length() > 1) {
        string fn = path_getsimple(mpath);
        if (walker.inSkippedNames(fn)) {
            LOGDEB("FsIndexer::indexFiles: skipping [" << path << "] (skpn)\n");
            return true;
        }

        string::size_type len = mpath.length();
        mpath = path_getfather(mpath);
        // getfather normally returns a path ending with /, getsimple 
        // would then return ''
        if (!mpath.empty() && mpath[mpath.size()-1] == '/')
            mpath.erase(mpath.size()-1);
        // should not be necessary, but lets be prudent. If the
        // path did not shorten, something is seriously amiss
        // (could be an assert actually)
        if (mpath.length() >= len)
            return true;
    }
    return false;
}

/** 
 * Index individual files, out of a full tree run. No database purging
 */
bool FsIndexer::indexFiles(list<string>& files, int flags)
{
    LOGDEB("FsIndexer::indexFiles\n");
    m_noretryfailed = (flags & ConfIndexer::IxFNoRetryFailed) != 0;
    m_cleancache = (flags & ConfIndexer::IxFCleanCache) != 0;
    bool ret = false;

    if (!init())
        return false;

    int abslen;
    if (m_config->getConfParam("idxabsmlen", &abslen))
        m_db->setAbstractParams(abslen, -1, -1);

    m_purgeCandidates.setRecord(true);

    // We use an FsTreeWalker just for handling the skipped path/name lists
    FsTreeWalker walker;
    walker.setSkippedPaths(m_config->getSkippedPaths());

    for (auto it = files.begin(); it != files.end(); ) {
        LOGDEB2("FsIndexer::indexFiles: [" << *it << "]\n");

        m_config->setKeyDir(path_getfather(*it));
        if (m_havelocalfields)
            localfieldsfromconf();

        bool follow{false};
        m_config->getConfParam("followLinks", &follow);

        walker.setOnlyNames(m_config->getOnlyNames());
        walker.setSkippedNames(m_config->getSkippedNames());
        // Check path against indexed areas and skipped names/paths
        if (!(flags & ConfIndexer::IxFIgnoreSkip) && 
            matchesSkipped(m_tdl, walker, *it)) {
            it++; 
            continue;
        }

        struct PathStat stb;
        int ststat = path_fileprops(*it, &stb, follow);
        if (ststat != 0) {
            LOGERR("FsIndexer::indexFiles: (l)stat " << *it << ": " <<
                   strerror(errno) << "\n");
            it++; 
            continue;
        }
        if (!(flags & ConfIndexer::IxFIgnoreSkip) &&
            (stb.pst_type == PathStat::PST_REGULAR ||
             stb.pst_type == PathStat::PST_SYMLINK)) {
            if (!walker.inOnlyNames(path_getsimple(*it))) {
                it++;
                continue;
            }
        }
        if (processone(*it, FsTreeWalker::FtwRegular, stb) != FsTreeWalker::FtwOk) {
            LOGERR("FsIndexer::indexFiles: processone failed\n");
            goto out;
        }
        it = files.erase(it);
    }

    ret = true;
out:
    shutdownQueues(ret);

    // Purge possible orphan documents
    if (ret == true) {
        LOGDEB("Indexfiles: purging orphans\n");
        for (const auto& udi : m_purgeCandidates.getCandidates()) {
            LOGDEB("Indexfiles: purging orphans for " << udi << "\n");
            m_db->purgeOrphans(udi);
        }
#ifdef IDX_THREADS
        m_db->waitUpdIdle();
#endif // IDX_THREADS
    }

    LOGDEB("FsIndexer::indexFiles: done\n");
    return ret;
}


/** Purge docs for given files out of the database */
bool FsIndexer::purgeFiles(list<string>& files)
{
    LOGDEB("FsIndexer::purgeFiles\n");
    bool ret = false;
    if (!init())
        return false;

    for (list<string>::iterator it = files.begin(); it != files.end(); ) {
        string udi;
        fileUdi::make_udi(*it, cstr_null, udi);
        // rcldb::purgefile returns true if the udi was either not
        // found or deleted, false only in case of actual error
        bool existed;
        if (!m_db->purgeFile(udi, &existed)) {
            LOGERR("FsIndexer::purgeFiles: Database error\n");
            goto out;
        }
        // If we actually deleted something, take it off the list
        if (existed) {
            it = files.erase(it);
        } else {
            it++;
        }
    }

    ret = true;
out:
    shutdownQueues(ret);
    LOGDEB("FsIndexer::purgeFiles: done\n");
    return ret;
}

// Local fields can be set for fs subtrees in the configuration file 
void FsIndexer::localfieldsfromconf()
{
    LOGDEB1("FsIndexer::localfieldsfromconf\n");

    string sfields;
    m_config->getConfParam("localfields", sfields);
    if (!sfields.compare(m_slocalfields)) 
        return;

    m_slocalfields = sfields;
    m_localfields.clear();
    if (sfields.empty())
        return;

    string value;
    ConfSimple attrs;
    m_config->valueSplitAttributes(sfields, value, attrs);
    vector<string> nmlst = attrs.getNames(cstr_null);
    for (const auto& anm : nmlst) {
        string nm = m_config->fieldCanon(anm);
        attrs.get(anm, m_localfields[nm]);
        LOGDEB2("FsIndexer::localfieldsfromconf: [" << nm << "]->[" <<
                m_localfields[nm] << "]\n");
    }
}

void FsIndexer::setlocalfields(const map<string, string>& fields, Rcl::Doc& doc)
{
    for (const auto& field : fields) {
        // Being chosen by the user, localfields override values from
        // the filter. The key is already canonic (see
        // localfieldsfromconf())
        doc.meta[field.first] = field.second;
    }
}

#ifdef IDX_THREADS
// Called updworker as seen from here, but the first step (and only in
// most meaningful configurations) is doing the word-splitting, which
// is why the task is referred as "Split" in the grand scheme of
// things. An other stage usually deals with the actual index update.
void *FsIndexerDbUpdWorker(void * fsp)
{
    recoll_threadinit();
    FsIndexer *fip = (FsIndexer*)fsp;
    WorkQueue<DbUpdTask*> *tqp = &fip->m_dwqueue;

    DbUpdTask *tsk;
    for (;;) {
        size_t qsz;
        if (!tqp->take(&tsk, &qsz)) {
            tqp->workerExit();
            return (void*)1;
        }
        LOGDEB0("FsIndexerDbUpdWorker: task ql " << qsz << "\n");
        if (!fip->m_db->addOrUpdate(tsk->udi, tsk->parent_udi, tsk->doc)) {
            LOGERR("FsIndexerDbUpdWorker: addOrUpdate failed\n");
            tqp->workerExit();
            return (void*)0;
        }
        delete tsk;
    }
}

void *FsIndexerInternfileWorker(void * fsp)
{
    recoll_threadinit();
    FsIndexer *fip = (FsIndexer*)fsp;
    WorkQueue<InternfileTask*> *tqp = &fip->m_iwqueue;
    RclConfig myconf(*(fip->m_stableconfig));

    InternfileTask *tsk{nullptr};
    for (;;) {
        if (!tqp->take(&tsk)) {
            tqp->workerExit();
            return (void*)1;
        }
        LOGDEB0("FsIndexerInternfileWorker: task fn " << tsk->fn << "\n");
        if (fip->processonefile(
                &myconf, tsk->fn, tsk->statbuf, tsk->localfields) != FsTreeWalker::FtwOk) {
            LOGERR("FsIndexerInternfileWorker: processone failed\n");
            tqp->workerExit();
            return (void*)0;
        }
        LOGDEB1("FsIndexerInternfileWorker: done fn " << tsk->fn << "\n");
        delete tsk;
    }
}
#endif // IDX_THREADS

/// This method gets called for every file and directory found by the
/// tree walker. 
///
/// It checks with the db if the file has changed and needs to be
/// reindexed. If so, it calls internfile() which will identify the
/// file type and call an appropriate handler to convert the document into
/// internal format, which we then add to the database.
///
/// Accent and majuscule handling are performed by the db module when doing
/// the actual indexing work. The Rcl::Doc created by internfile()
/// mostly contains pretty raw utf8 data.
FsTreeWalker::Status FsIndexer::processone(
    const std::string &fn, FsTreeWalker::CbFlag flg, const struct PathStat& stp)
{
    if (!statusUpdater()->update(DbIxStatus::DBIXS_FILES, fn)) {
        return FsTreeWalker::FtwStop;
    }

    // If we're changing directories, possibly adjust parameters (set
    // the current directory in configuration object)
    if (flg == FsTreeWalker::FtwDirEnter || flg == FsTreeWalker::FtwDirReturn) {
        m_config->setKeyDir(fn);
        // Set up filter/skipped patterns for this subtree. 
        m_walker.setOnlyNames(m_config->getOnlyNames());
        m_walker.setSkippedNames(m_config->getSkippedNames());
        // Adjust local fields from config for this subtree
        if (m_havelocalfields)
            localfieldsfromconf();
        if (flg == FsTreeWalker::FtwDirReturn)
            return FsTreeWalker::FtwOk;
    }
    if (flg == FsTreeWalker::FtwSkipped) {
        IdxDiags::theDiags().record(IdxDiags::Skipped, fn);
        return FsTreeWalker::FtwOk;
    }
#ifdef IDX_THREADS
    if (m_haveInternQ) {
        InternfileTask *tp = new InternfileTask(fn, stp, m_localfields);
        if (m_iwqueue.put(tp)) {
            return FsTreeWalker::FtwOk;
        } else {
            return FsTreeWalker::FtwError;
        }
    }
#endif

    return processonefile(m_config, fn, stp, m_localfields);
}

// Start db update, either by queueing or by direct call
bool FsIndexer::launchAddOrUpdate(const string& udi, const string& parent_udi, Rcl::Doc& doc)
{
    trimmeta(doc.meta);
#ifdef IDX_THREADS
    if (m_haveSplitQ) {
        DbUpdTask *tp = new DbUpdTask(udi, parent_udi, doc);
        if (!m_dwqueue.put(tp)) {
            LOGERR("processonefile: wqueue.put failed\n");
            return false;
        } else {
            return true;
        }
    }
#endif

    return m_db->addOrUpdate(udi, parent_udi, doc);
}

FsTreeWalker::Status FsIndexer::processonefile(
    RclConfig *config, const std::string &fn, const struct PathStat& stp,
    const map<string, string>& localfields)
{
    ////////////////////
    // Check db up to date ? Doing this before file type
    // identification means that, if usesystemfilecommand is switched
    // from on to off it may happen that some files which are now
    // without mime type will not be purged from the db, resulting
    // in possible 'cannot intern file' messages at query time...

    // This is needed if we are in a separate thread than processone()
    // (mostly always when multithreading). Needed esp. for
    // excludedmimetypes, etc.
    config->setKeyDir(path_getfather(fn));
    
    // File signature and up to date check. The sig is based on
    // m/ctime and size and the possibly new value is checked against
    // the stored one.
    string sig;
    fsmakesig(stp, sig);
    string udi;
    fileUdi::make_udi(fn, cstr_null, udi);
    unsigned int existingDoc;
    string oldsig;
    bool needupdate;
    if (m_noretryfailed) {
        needupdate = m_db->needUpdate(udi, sig, &existingDoc, &oldsig);
    } else {
        needupdate = m_db->needUpdate(udi, sig, &existingDoc, nullptr);
    }

    // If ctime (which we use for the sig) differs from mtime, then at most
    // the extended attributes were changed, no need to index content.
    // This unfortunately leaves open the case where the data was
    // modified, then the extended attributes, in which case we will
    // miss the data update. We would have to store both the mtime and
    // the ctime to avoid this
    bool xattronly = m_detectxattronly && !m_db->inFullReset() && 
        existingDoc && needupdate && (stp.pst_mtime < stp.pst_ctime);

    LOGDEB("processone: needupdate " << needupdate << " noretry " <<
           m_noretryfailed << " existing " << existingDoc << " oldsig [" <<
           oldsig << "]\n");

    // If noretryfailed is set, check for a file which previously
    // failed to index, and avoid re-processing it
    if (needupdate && m_noretryfailed && existingDoc && 
        !oldsig.empty() && oldsig.back() == '+') {
        // Check that the sigs are the same except for the '+'. If the file
        // actually changed, we always retry (maybe it was fixed)
        string nold = oldsig.substr(0, oldsig.size()-1);
        if (!nold.compare(sig)) {
            LOGDEB("processone: not retrying previously failed file\n");
            m_db->setExistingFlags(udi, existingDoc);
            needupdate = false;
        }
    }

    if (!needupdate) {
        LOGDEB0("processone: up to date: " << fn << "\n");
        if (!statusUpdater()->update(
                DbIxStatus::DBIXS_FILES, fn, DbIxStatusUpdater::IncrFilesDone)) {
            return FsTreeWalker::FtwStop;
        }
        return FsTreeWalker::FtwOk;
    }

    LOGDEB0("processone: processing: [" << displayableBytes(stp.pst_size) << "] " << fn << "\n");

    // Note that we used to do the full path here, but I ended up
    // believing that it made more sense to use only the file name
    string utf8fn = compute_utf8fn(config, fn, true);

    // parent_udi is initially the same as udi, it will be used if there 
    // are subdocs.
    string parent_udi = udi;

    Rcl::Doc doc;

    std::string ascdate;
    lltodecstr(stp.pst_mtime, ascdate);

#ifdef EXT4_BIRTH_TIME
    std::string brdate;
    if (stp.pst_btime) {
        // Note that btime==0 is a valid date. At the moment though we reserve it for "no value"
        // TBD if this is acceptable or not
        lltodecstr(stp.pst_btime, brdate);
    }
#endif

    bool hadNullIpath = false;
    string mimetype;

    if (!xattronly) {
        FileInterner interner(fn, stp, config, FileInterner::FIF_none);
        if (!interner.ok()) {
            // no indexing whatsoever in this case. This typically means that
            // indexallfilenames is not set
            return FsTreeWalker::FtwOk;
        }
        mimetype = interner.getMimetype();

        interner.setMissingStore(m_missing);
        FileInterner::Status fis = FileInterner::FIAgain;
        bool hadNonNullIpath = false;
        while (fis == FileInterner::FIAgain) {
            doc.erase();
            try {
                fis = interner.internfile(doc);
            } catch (CancelExcept) {
                LOGERR("fsIndexer::processone: interrupted\n");
                return FsTreeWalker::FtwStop;
            }

            // We index at least the file name even if there was an error.
            // We'll change the signature to ensure that the indexing will
            // be retried every time.
            
            // If there is an error and the base doc was already seen,
            // we're done
            if (fis == FileInterner::FIError && hadNullIpath) {
                return FsTreeWalker::FtwOk;
            }
            
            // Internal access path for multi-document files. If empty, this is
            // for the main file.
            if (doc.ipath.empty()) {
                hadNullIpath = true;
                if (hadNonNullIpath) {
                    // Note that only the filters can reliably compute
                    // this. What we do is dependant of the doc order (if
                    // we see the top doc first, we won't set the flag)
                    doc.haschildren = true;
                }
            } else {
                hadNonNullIpath = true;
            }
            fileUdi::make_udi(fn, doc.ipath, udi);

            // Set file name, mod time and url if not done by
            // filter. We used to set the top-level container file
            // name for all subdocs without a proper file name, but
            // this did not make sense (resulted in multiple not
            // useful hits on the subdocs when searching for the
            // file name).
            if (doc.fmtime.empty())
                doc.fmtime = ascdate;

#ifdef EXT4_BIRTH_TIME
            if (!brdate.empty() && !doc.hasmetavalue(Rcl::Doc::keybrt) ) {
                doc.meta[Rcl::Doc::keybrt] = brdate;
            }
 #endif   
            if (doc.url.empty())
                doc.url = path_pathtofileurl(fn);

            if (doc.ipath.empty() && !doc.hasmetavalue(Rcl::Doc::keyfn)) {
                doc.meta[Rcl::Doc::keyfn] = utf8fn;
            } 
            // Set container file name for all docs, top or subdoc
            doc.meta[Rcl::Doc::keyctfn] = utf8fn;

            doc.pcbytes = lltodecstr(stp.pst_size);
            // Document signature for up to date checks. All subdocs inherit the
            // file's.
            doc.sig = sig;

            // If there was an error, ensure indexing will be
            // retried. This is for the once missing, later installed
            // filter case. It can make indexing much slower (if there are
            // myriads of such files, the ext script is executed for them
            // and fails every time)
            if (fis == FileInterner::FIError) {
                IdxDiags::theDiags().record(IdxDiags::Error, fn, doc.ipath);
                doc.sig += cstr_plus;
            }

            // Possibly add fields from local config
            if (m_havelocalfields) 
                setlocalfields(localfields, doc);

            // Add document to database. If there is an ipath, add it
            // as a child of the file document.
            if (!launchAddOrUpdate(udi, doc.ipath.empty() ? cstr_null : parent_udi, doc)) {
                return FsTreeWalker::FtwError;
            } 

            // Tell what we are doing and check for interrupt request
            int incr = DbIxStatusUpdater::IncrDocsDone;
            std::string sfn(fn);
            if (!doc.ipath.empty()) {
                sfn += "|" + doc.ipath;
            } else {
                if (fis == FileInterner::FIError) {
                    incr |= DbIxStatusUpdater::IncrFileErrors;
                }
                incr |= DbIxStatusUpdater::IncrFilesDone;
            }
            if (!statusUpdater()->update(DbIxStatus::DBIXS_FILES, sfn, incr)) {
                return FsTreeWalker::FtwStop;
            }

        }

        if (fis == FileInterner::FIError) {
            // In case of error, avoid purging any existing
            // subdoc. For example on windows, this will avoid erasing
            // all the emails from a .ost because it is currently
            // locked by Outlook.
            LOGDEB("processonefile: internfile error, marking "
                   "subdocs as existing\n");
            m_db->udiTreeMarkExisting(parent_udi);
        } else {
            // If this doc existed and it's a container, recording for
            // possible subdoc purge (this will be used only if we don't do a
            // db-wide purge, e.g. if we're called from indexfiles()).
            LOGDEB2("processOnefile: existingDoc " << existingDoc <<
                    " hadNonNullIpath " << hadNonNullIpath << "\n");
            if (existingDoc && hadNonNullIpath) {
                m_purgeCandidates.record(parent_udi);
            }
        }
#if defined(HAVE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
        // See framagit issue 26. This is off by default and controlled by a command line switch.
        if (m_cleancache) {
            int fd = open(fn.c_str(), O_RDONLY);
            if (fd >= 0) {
                if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)) {
                    LOGSYSERR("processonefile", "posix_fadvise", fn);
                }
                close(fd);
            }
        }
#endif
    }

    // If we had no instance with a null ipath, we create an empty
    // document to stand for the file itself, to be used mainly for up
    // to date checks. Typically this happens for an mbox file.
    //
    // If xattronly is set, ONLY the extattr metadata is valid and will be used
    // by the following step.
    if (xattronly || hadNullIpath == false) {
        LOGDEB("Creating empty doc for file or pure xattr update\n");
        Rcl::Doc fileDoc;
        if (xattronly) {
            map<string, string> xfields;
            reapXAttrs(config, fn, xfields);
            docFieldsFromXattrs(config, xfields, fileDoc);
            fileDoc.metaonly = true;
        } else {
            fileDoc.fmtime = ascdate;
#ifdef EXT4_BIRTH_TIME
            if (!brdate.empty()) 
                fileDoc.meta[Rcl::Doc::keybrt] = brdate;
#endif
            fileDoc.meta[Rcl::Doc::keyfn] = fileDoc.meta[Rcl::Doc::keyctfn] = utf8fn;
            fileDoc.haschildren = true;
            fileDoc.mimetype = mimetype;
            fileDoc.url = path_pathtofileurl(fn);
            if (m_havelocalfields) 
                setlocalfields(localfields, fileDoc);
            fileDoc.pcbytes = lltodecstr(stp.pst_size);
        }

        fileDoc.sig = sig;

        if (!launchAddOrUpdate(parent_udi, cstr_null, fileDoc)) {
            return FsTreeWalker::FtwError;
        }
    }

    return FsTreeWalker::FtwOk;
}

