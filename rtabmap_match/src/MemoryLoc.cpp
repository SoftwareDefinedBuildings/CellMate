#include <rtabmap/utilite/UEventsManager.h>
#include <rtabmap/utilite/ULogger.h>
#include <rtabmap/utilite/UTimer.h>
#include <rtabmap/utilite/UConversion.h>
#include <rtabmap/utilite/UProcessInfo.h>
#include <rtabmap/utilite/UMath.h>

#include <rtabmap/core/Signature.h>
#include <rtabmap/core/Parameters.h>
#include <rtabmap/core/RtabmapEvent.h>
#include <rtabmap/core/VWDictionary.h>
#include <rtabmap/core/EpipolarGeometry.h>
#include <rtabmap/core/VisualWord.h>
#include <rtabmap/core/Features2d.h>
#include <rtabmap/core/RegistrationIcp.h>
#include <rtabmap/core/Registration.h>
#include <rtabmap/core/RegistrationVis.h>
#include <rtabmap/core/DBDriver.h>
#include <rtabmap/core/util3d_features.h>
#include <rtabmap/core/util3d_filtering.h>
#include <rtabmap/core/util3d_correspondences.h>
#include <rtabmap/core/util3d_registration.h>
#include <rtabmap/core/util3d_surface.h>
#include <rtabmap/core/util3d_transforms.h>
#include <rtabmap/core/util3d_motion_estimation.h>
#include <rtabmap/core/util3d.h>
#include <rtabmap/core/util2d.h>
#include <rtabmap/core/Statistics.h>
#include <rtabmap/core/Compression.h>
#include <rtabmap/core/Graph.h>
#include <rtabmap/core/Stereo.h>

#include <pcl/io/pcd_io.h>
#include <pcl/common/common.h>

#include "MemoryLoc.h"

const int MemoryLoc::kIdStart = 0;
const int MemoryLoc::kIdVirtual = -1;
const int MemoryLoc::kIdInvalid = 0;

MemoryLoc::MemoryLoc(const ParametersMap &parameters) :
    _dbDriver(0),
    _similarityThreshold(Parameters::defaultMemRehearsalSimilarity()),
    _binDataKept(Parameters::defaultMemBinDataKept()),
    _rawDescriptorsKept(Parameters::defaultMemRawDescriptorsKept()),
    _saveDepth16Format(Parameters::defaultMemSaveDepth16Format()),
    _notLinkedNodesKeptInDb(Parameters::defaultMemNotLinkedNodesKept()),
    _incrementalMemory(Parameters::defaultMemIncrementalMemory()),
    _reduceGraph(Parameters::defaultMemReduceGraph()),
    _maxStMemSize(Parameters::defaultMemSTMSize()),
    _recentWmRatio(Parameters::defaultMemRecentWmRatio()),
    _transferSortingByWeightId(Parameters::defaultMemTransferSortingByWeightId()),
    _idUpdatedToNewOneRehearsal(Parameters::defaultMemRehearsalIdUpdatedToNewOne()),
    _generateIds(Parameters::defaultMemGenerateIds()),
    _badSignaturesIgnored(Parameters::defaultMemBadSignaturesIgnored()),
    _mapLabelsAdded(Parameters::defaultMemMapLabelsAdded()),
    _imageDecimation(Parameters::defaultMemImageDecimation()),
    _laserScanDownsampleStepSize(Parameters::defaultMemLaserScanDownsampleStepSize()),
    _reextractLoopClosureFeatures(Parameters::defaultRGBDLoopClosureReextractFeatures()),
    _rehearsalMaxDistance(Parameters::defaultRGBDLinearUpdate()),
    _rehearsalMaxAngle(Parameters::defaultRGBDAngularUpdate()),
    _rehearsalWeightIgnoredWhileMoving(Parameters::defaultMemRehearsalWeightIgnoredWhileMoving()),
    _useOdometryFeatures(Parameters::defaultMemUseOdomFeatures()),
    _idCount(kIdStart),
    _idMapCount(kIdStart),
    _lastSignature(0),
    _lastGlobalLoopClosureId(0),
    _memoryChanged(false),
    _linksChanged(false),
    _signaturesAdded(0),

    _badSignRatio(Parameters::defaultKpBadSignRatio()),
    _tfIdfLikelihoodUsed(Parameters::defaultKpTfIdfLikelihoodUsed()),
    _parallelized(Parameters::defaultKpParallelized()),

    _minInliers(rtabmap::Parameters::defaultVisMinInliers()),
    _iterations(rtabmap::Parameters::defaultVisIterations()),
    _pnpRefineIterations(rtabmap::Parameters::defaultVisPnPRefineIterations()),
    _pnpReprojError(rtabmap::Parameters::defaultVisPnPReprojError()),
    _pnpFlags(rtabmap::Parameters::defaultVisPnPFlags())
{
    _feature2D = Feature2D::create(parameters);
    _vwd = new VWDictionary(parameters);
    _registrationPipeline = Registration::create(parameters);
    _registrationIcp = new RegistrationIcp(parameters);
    this->parseParameters(parameters);
}

bool MemoryLoc::init(const std::string &dbUrl, bool dbOverwritten, const ParametersMap &parameters, bool postInitClosingEvents)
{
    if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(RtabmapEventInit::kInitializing));

    UDEBUG("");
    this->parseParameters(parameters);
    bool loadAllNodesInWM = Parameters::defaultMemInitWMWithAllNodes();
    Parameters::parse(parameters, Parameters::kMemInitWMWithAllNodes(), loadAllNodesInWM);

    if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit("Clearing memory..."));
    DBDriver *tmpDriver = 0;
    if ((!_memoryChanged && !_linksChanged) || dbOverwritten)
    {
        if (_dbDriver)
        {
            tmpDriver = _dbDriver;
            _dbDriver = 0; // HACK for the clear() below to think that there is no db
        }
    }
    else if (!_memoryChanged && _linksChanged)
    {
        _dbDriver->setTimestampUpdateEnabled(false); // update links only
    }
    this->clear();
    if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit("Clearing memory, done!"));

    if (tmpDriver)
    {
        _dbDriver = tmpDriver;
    }

    if (_dbDriver)
    {
        if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit("Closing database connection..."));
        _dbDriver->closeConnection();
        if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit("Closing database connection, done!"));
    }

    if (_dbDriver == 0 && !dbUrl.empty())
    {
        _dbDriver = DBDriver::create(parameters);
    }

    bool success = true;
    if (_dbDriver)
    {
        _dbDriver->setTimestampUpdateEnabled(true); // make sure that timestamp update is enabled (may be disabled above)
        success = false;
        if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(std::string("Connecting to database ") + dbUrl + "..."));
        if (_dbDriver->openConnection(dbUrl, dbOverwritten))
        {
            success = true;
            if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(std::string("Connecting to database ") + dbUrl + ", done!"));

            // Load the last working memory...
            std::list<Signature *> dbSignatures;

            if (loadAllNodesInWM)
            {
                if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(std::string("Loading all nodes to WM...")));
                std::set<int> ids;
                _dbDriver->getAllNodeIds(ids, true);
                _dbDriver->loadSignatures(std::list<int>(ids.begin(), ids.end()), dbSignatures);
            }
            else
            {
                // load previous session working memory
                if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(std::string("Loading last nodes to WM...")));
                _dbDriver->loadLastNodes(dbSignatures);
            }
            for (std::list<Signature *>::reverse_iterator iter = dbSignatures.rbegin(); iter != dbSignatures.rend(); ++iter)
            {
                // ignore bad signatures
                if (!((*iter)->isBadSignature() && _badSignaturesIgnored))
                {
                    // insert all in WM
                    // Note: it doesn't make sense to keep last STM images
                    //       of the last session in the new STM because they can be
                    //       only linked with the ones of the current session by
                    //       global loop closures.
                    _signatures.insert(std::pair<int, Signature *>((*iter)->id(), *iter));
                    _workingMem.insert(std::make_pair((*iter)->id(), UTimer::now()));
                }
                else
                {
                    delete *iter;
                }
            }
            if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(std::string("Loading nodes to WM, done! (") + uNumber2Str(int(_workingMem.size() + _stMem.size())) + " loaded)"));

            // Assign the last signature
            if (_stMem.size() > 0)
            {
                _lastSignature = uValue(_signatures, *_stMem.rbegin(), (Signature *)0);
            }
            else if (_workingMem.size() > 0)
            {
                _lastSignature = uValue(_signatures, _workingMem.rbegin()->first, (Signature *)0);
            }

            // Last id
            _dbDriver->getLastNodeId(_idCount);
            _idMapCount = _lastSignature ? _lastSignature->mapId() + 1 : kIdStart;
        }
        else
        {
            if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(RtabmapEventInit::kError, std::string("Connecting to database ") + dbUrl + ", path is invalid!"));
        }
    }
    else
    {
        _idCount = kIdStart;
        _idMapCount = kIdStart;
    }

    _workingMem.insert(std::make_pair(kIdVirtual, 0));

    UDEBUG("ids start with %d", _idCount + 1);
    UDEBUG("map ids start with %d", _idMapCount);


    // Now load the dictionary if we have a connection
    if (_dbDriver && _dbDriver->isConnected())
    {
        if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit("Loading dictionary..."));
        if (loadAllNodesInWM)
        {
            // load all referenced words in working memory
            std::set<int> wordIds;
            const std::map<int, Signature *> &signatures = this->getSignatures();
            for (std::map<int, Signature *>::const_iterator i = signatures.begin(); i != signatures.end(); ++i)
            {
                const std::multimap<int, cv::KeyPoint> &words = i->second->getWords();
                std::list<int> keys = uUniqueKeys(words);
                wordIds.insert(keys.begin(), keys.end());
            }
            if (wordIds.size())
            {
                std::list<VisualWord *> words;
                _dbDriver->loadWords(wordIds, words);
                for (std::list<VisualWord *>::iterator iter = words.begin(); iter != words.end(); ++iter)
                {
                    _vwd->addWord(*iter);
                }
                // Get Last word id
                int id = 0;
                _dbDriver->getLastWordId(id);
                _vwd->setLastWordId(id);
            }
        }
        else
        {
            // load the last dictionary
            _dbDriver->load(_vwd);
        }
        UDEBUG("%d words loaded!", _vwd->getUnusedWordsSize());
        _vwd->update();
        if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(uFormat("Loading dictionary, done! (%d words)", (int)_vwd->getUnusedWordsSize())));
    }

    if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(std::string("Adding word references...")));
    // Enable loaded signatures
    const std::map<int, Signature *> &signatures = this->getSignatures();
    for (std::map<int, Signature *>::const_iterator i = signatures.begin(); i != signatures.end(); ++i)
    {
        Signature *s = this->_getSignature(i->first);
        UASSERT(s != 0);

        const std::multimap<int, cv::KeyPoint> &words = s->getWords();
        if (words.size())
        {
            UDEBUG("node=%d, word references=%d", s->id(), words.size());
            for (std::multimap<int, cv::KeyPoint>::const_iterator iter = words.begin(); iter != words.end(); ++iter)
            {
                _vwd->addWordRef(iter->first, i->first);
            }
            s->setEnabled(true);
        }
    }
    if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(uFormat("Adding word references, done! (%d)", _vwd->getTotalActiveReferences())));

    if (_vwd->getUnusedWordsSize())
    {
        UWARN("_vwd->getUnusedWordsSize() must be empty... size=%d", _vwd->getUnusedWordsSize());
    }
    UDEBUG("Total word references added = %d", _vwd->getTotalActiveReferences());

    if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(RtabmapEventInit::kInitialized));
    return success;
}

void MemoryLoc::close(bool databaseSaved, bool postInitClosingEvents)
{
    UINFO("databaseSaved=%d, postInitClosingEvents=%d", databaseSaved ? 1 : 0, postInitClosingEvents ? 1 : 0);
    if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(RtabmapEventInit::kClosing));

    if (!databaseSaved || (!_memoryChanged && !_linksChanged))
    {
        if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(uFormat("No changes added to database.")));

        UINFO("No changes added to database.");
        if (_dbDriver)
        {
            if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(uFormat("Closing database \"%s\"...", _dbDriver->getUrl().c_str())));
            _dbDriver->closeConnection();
            delete _dbDriver;
            _dbDriver = 0;
            if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit("Closing database, done!"));
        }
        if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit("Clearing memory..."));
        this->clear();
        if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit("Clearing memory, done!"));
    }
    else
    {
        UINFO("Saving memory...");
        if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit("Saving memory..."));
        if (!_memoryChanged && _linksChanged && _dbDriver)
        {
            // don't update the time stamps!
            UDEBUG("");
            _dbDriver->setTimestampUpdateEnabled(false);
        }
        this->clear();
        if (_dbDriver)
        {
            _dbDriver->emptyTrashes();
            if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit("Saving memory, done!"));
            if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(uFormat("Closing database \"%s\"...", _dbDriver->getUrl().c_str())));
            _dbDriver->closeConnection();
            delete _dbDriver;
            _dbDriver = 0;
            if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit("Closing database, done!"));
        }
        else
        {
            if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit("Saving memory, done!"));
        }
    }
    if (postInitClosingEvents) UEventsManager::post(new RtabmapEventInit(RtabmapEventInit::kClosed));
}

MemoryLoc::~MemoryLoc()
{
    this->close();

    if (_dbDriver)
    {
        UWARN("Please call MemoryLoc::close() before");
    }
    if (_feature2D)
    {
        delete _feature2D;
    }
    if (_vwd)
    {
        delete _vwd;
    }
    if (_registrationPipeline)
    {
        delete _registrationPipeline;
    }
    if (_registrationIcp)
    {
        delete _registrationIcp;
    }
}

void MemoryLoc::parseParameters(const ParametersMap &parameters)
{
    uInsert(parameters_, parameters);

    UDEBUG("");
    ParametersMap::const_iterator iter;

    Parameters::parse(parameters, Parameters::kMemBinDataKept(), _binDataKept);
    Parameters::parse(parameters, Parameters::kMemRawDescriptorsKept(), _rawDescriptorsKept);
    Parameters::parse(parameters, Parameters::kMemSaveDepth16Format(), _saveDepth16Format);
    Parameters::parse(parameters, Parameters::kMemReduceGraph(), _reduceGraph);
    Parameters::parse(parameters, Parameters::kMemNotLinkedNodesKept(), _notLinkedNodesKeptInDb);
    Parameters::parse(parameters, Parameters::kMemRehearsalIdUpdatedToNewOne(), _idUpdatedToNewOneRehearsal);
    Parameters::parse(parameters, Parameters::kMemGenerateIds(), _generateIds);
    Parameters::parse(parameters, Parameters::kMemBadSignaturesIgnored(), _badSignaturesIgnored);
    Parameters::parse(parameters, Parameters::kMemMapLabelsAdded(), _mapLabelsAdded);
    Parameters::parse(parameters, Parameters::kMemRehearsalSimilarity(), _similarityThreshold);
    Parameters::parse(parameters, Parameters::kMemRecentWmRatio(), _recentWmRatio);
    Parameters::parse(parameters, Parameters::kMemTransferSortingByWeightId(), _transferSortingByWeightId);
    Parameters::parse(parameters, Parameters::kMemSTMSize(), _maxStMemSize);
    Parameters::parse(parameters, Parameters::kMemImageDecimation(), _imageDecimation);
    Parameters::parse(parameters, Parameters::kMemLaserScanDownsampleStepSize(), _laserScanDownsampleStepSize);
    Parameters::parse(parameters, Parameters::kRGBDLoopClosureReextractFeatures(), _reextractLoopClosureFeatures);
    Parameters::parse(parameters, Parameters::kRGBDLinearUpdate(), _rehearsalMaxDistance);
    Parameters::parse(parameters, Parameters::kRGBDAngularUpdate(), _rehearsalMaxAngle);
    Parameters::parse(parameters, Parameters::kMemRehearsalWeightIgnoredWhileMoving(), _rehearsalWeightIgnoredWhileMoving);
    Parameters::parse(parameters, Parameters::kMemUseOdomFeatures(), _useOdometryFeatures);

    UASSERT_MSG(_maxStMemSize >= 0, uFormat("value=%d", _maxStMemSize).c_str());
    UASSERT_MSG(_similarityThreshold >= 0.0f && _similarityThreshold <= 1.0f, uFormat("value=%f", _similarityThreshold).c_str());
    UASSERT_MSG(_recentWmRatio >= 0.0f && _recentWmRatio <= 1.0f, uFormat("value=%f", _recentWmRatio).c_str());
    UASSERT(_imageDecimation >= 1);
    UASSERT(_rehearsalMaxDistance >= 0.0f);
    UASSERT(_rehearsalMaxAngle >= 0.0f);

    if (_dbDriver)
    {
        _dbDriver->parseParameters(parameters);
    }

    // Keypoint stuff
    if (_vwd)
    {
        _vwd->parseParameters(parameters);
    }

    Parameters::parse(parameters, Parameters::kKpTfIdfLikelihoodUsed(), _tfIdfLikelihoodUsed);
    Parameters::parse(parameters, Parameters::kKpParallelized(), _parallelized);
    Parameters::parse(parameters, Parameters::kKpBadSignRatio(), _badSignRatio);

    //Keypoint detector
    UASSERT(_feature2D != 0);
    Feature2D::Type detectorStrategy = Feature2D::kFeatureUndef;
    if ((iter = parameters.find(Parameters::kKpDetectorStrategy())) != parameters.end())
    {
        detectorStrategy = (Feature2D::Type)std::atoi((*iter).second.c_str());
    }
    if (detectorStrategy != Feature2D::kFeatureUndef)
    {
        UDEBUG("new detector strategy %d", int(detectorStrategy));
        if (_feature2D)
        {
            delete _feature2D;
            _feature2D = 0;
        }

        _feature2D = Feature2D::create(detectorStrategy, parameters_);
    }
    else if (_feature2D)
    {
        _feature2D->parseParameters(parameters);
    }

    Registration::Type regStrategy = Registration::kTypeUndef;
    if ((iter = parameters.find(Parameters::kRegStrategy())) != parameters.end())
    {
        regStrategy = (Registration::Type)std::atoi((*iter).second.c_str());
    }
    if (regStrategy != Registration::kTypeUndef)
    {
        UDEBUG("new registration strategy %d", int(regStrategy));
        if (_registrationPipeline)
        {
            delete _registrationPipeline;
            _registrationPipeline = 0;
        }

        _registrationPipeline = Registration::create(regStrategy, parameters_);
    }
    else if (_registrationPipeline)
    {
        _registrationPipeline->parseParameters(parameters);
    }

    if (_registrationIcp)
    {
        _registrationIcp->parseParameters(parameters);
    }

    // do this after all parameters are parsed
    // SLAM mode vs Localization mode
    iter = parameters.find(Parameters::kMemIncrementalMemory());
    if (iter != parameters.end())
    {
        bool value = uStr2Bool(iter->second.c_str());
        if (value == false && _incrementalMemory)
        {
            // From SLAM to localization, change map id
            this->incrementMapId();

            // The easiest way to make sure that the mapping session is saved
            // is to save the memory in the database and reload it.
            if ((_memoryChanged || _linksChanged) && _dbDriver)
            {
                UWARN("Switching from Mapping to Localization mode, the database will be saved and reloaded.");
                this->init(_dbDriver->getUrl());
            }
        }
        _incrementalMemory = value;
    }

    rtabmap::Parameters::parse(parameters, rtabmap::Parameters::kVisMinInliers(), _minInliers);
    rtabmap::Parameters::parse(parameters, rtabmap::Parameters::kVisIterations(), _iterations);
    rtabmap::Parameters::parse(parameters, rtabmap::Parameters::kVisPnPRefineIterations(), _pnpRefineIterations);
    rtabmap::Parameters::parse(parameters, rtabmap::Parameters::kVisPnPReprojError(), _pnpReprojError);
    rtabmap::Parameters::parse(parameters, rtabmap::Parameters::kVisPnPFlags(), _pnpFlags);
}

void MemoryLoc::preUpdate()
{
    _signaturesAdded = 0;
    this->cleanUnusedWords();
    if (_vwd && !_parallelized)
    {
        //When parallelized, it is done in CreateSignature
        _vwd->update();
    }
}

bool MemoryLoc::update(
    const SensorData &data,
    Statistics *stats)
{
    return update(data, Transform(), cv::Mat(), stats);
}

bool MemoryLoc::update(
    const SensorData &data,
    const Transform &pose,
    const cv::Mat &covariance,
    Statistics *stats)
{
    UDEBUG("");
    UTimer timer;
    UTimer totalTimer;
    timer.start();
    float t;

    //============================================================
    // Pre update...
    //============================================================
    UDEBUG("pre-updating...");
    this->preUpdate();
    t = timer.ticks() * 1000;
    if (stats) stats->addStatistic(Statistics::kTimingMemPre_update(), t);
    UDEBUG("time preUpdate=%f ms", t);

    //============================================================
    // Create a signature with the image received.
    //============================================================
    Signature *signature = this->createSignature(data, pose, stats);
    if (signature == 0)
    {
        UERROR("Failed to create a signature...");
        return false;
    }

    t = timer.ticks() * 1000;
    if (stats) stats->addStatistic(Statistics::kTimingMemSignature_creation(), t);
    UDEBUG("time creating signature=%f ms", t);

    // It will be added to the short-term memory, no need to delete it...
    this->addSignatureToStm(signature, covariance);

    _lastSignature = signature;

    //============================================================
    // Rehearsal step...
    // Compare with the X last signatures. If different, add this
    // signature like a parent to the memory tree, otherwise add
    // it as a child to the similar signature.
    //============================================================
    if (_incrementalMemory)
    {
        if (_similarityThreshold < 1.0f)
        {
            this->rehearsal(signature, stats);
        }
        t = timer.ticks() * 1000;
        if (stats) stats->addStatistic(Statistics::kTimingMemRehearsal(), t);
        UDEBUG("time rehearsal=%f ms", t);
    }
    else
    {
        if (_workingMem.size() <= 1)
        {
            UWARN("The working memory is empty and the memory is not "
                  "incremental (Mem/IncrementalMemory=False), no loop closure "
                  "can be detected! Please set Mem/IncrementalMemory=true to increase "
                  "the memory with new images or decrease the STM size (which is %d "
                  "including the new one added).", (int)_stMem.size());
        }
    }

    //============================================================
    // Transfer the oldest signature of the short-term memory to the working memory
    //============================================================
    int notIntermediateNodesCount = 0;
    for (std::set<int>::iterator iter = _stMem.begin(); iter != _stMem.end(); ++iter)
    {
        const Signature *s = this->getSignature(*iter);
        UASSERT(s != 0);
        if (s->getWeight() >= 0)
        {
            ++notIntermediateNodesCount;
        }
    }
    std::map<int, int> reducedIds;
    while (_stMem.size() && _maxStMemSize > 0 && notIntermediateNodesCount > _maxStMemSize)
    {
        int id = *_stMem.begin();
        Signature *s = this->_getSignature(id);
        UASSERT(s != 0);
        if (s->getWeight() >= 0)
        {
            --notIntermediateNodesCount;
        }

        int reducedTo = 0;
        moveSignatureToWMFromSTM(id, &reducedTo);

        if (reducedTo > 0)
        {
            reducedIds.insert(std::make_pair(id, reducedTo));
        }
    }
    if (stats) stats->setReducedIds(reducedIds);

    if (!_memoryChanged && _incrementalMemory)
    {
        _memoryChanged = true;
    }

    UDEBUG("totalTimer = %fs", totalTimer.ticks());

    return true;
}

void MemoryLoc::addSignatureToStm(Signature *signature, const cv::Mat &covariance)
{
    UTimer timer;
    // add signature on top of the short-term memory
    if (signature)
    {
        UDEBUG("adding %d", signature->id());
        // Update neighbors
        if (_stMem.size())
        {
            if (_signatures.at(*_stMem.rbegin())->mapId() == signature->mapId())
            {
                Transform motionEstimate;
                if (!signature->getPose().isNull() &&
                        !_signatures.at(*_stMem.rbegin())->getPose().isNull())
                {
                    cv::Mat infMatrix = covariance.inv();
                    motionEstimate = _signatures.at(*_stMem.rbegin())->getPose().inverse() * signature->getPose();
                    _signatures.at(*_stMem.rbegin())->addLink(Link(*_stMem.rbegin(), signature->id(), Link::kNeighbor, motionEstimate, infMatrix));
                    signature->addLink(Link(signature->id(), *_stMem.rbegin(), Link::kNeighbor, motionEstimate.inverse(), infMatrix));
                }
                else
                {
                    _signatures.at(*_stMem.rbegin())->addLink(Link(*_stMem.rbegin(), signature->id(), Link::kNeighbor, Transform()));
                    signature->addLink(Link(signature->id(), *_stMem.rbegin(), Link::kNeighbor, Transform()));
                }
                UDEBUG("Min STM id = %d", *_stMem.begin());
            }
            else
            {
                UDEBUG("Ignoring neighbor link between %d and %d because they are not in the same map! (%d vs %d)",
                       *_stMem.rbegin(), signature->id(),
                       _signatures.at(*_stMem.rbegin())->mapId(), signature->mapId());

                //Tag the first node of the map
                std::string tag = uFormat("map%d", signature->mapId());
                if (getSignatureIdByLabel(tag, false) == 0)
                {
                    UINFO("Tagging node %d with label \"%s\"", signature->id(), tag.c_str());
                    signature->setLabel(tag);
                }
            }
        }
        else if (_mapLabelsAdded)
        {
            //Tag the first node of the map
            std::string tag = uFormat("map%d", signature->mapId());
            if (getSignatureIdByLabel(tag, false) == 0)
            {
                UINFO("Tagging node %d with label \"%s\"", signature->id(), tag.c_str());
                signature->setLabel(tag);
            }
        }

        _signatures.insert(_signatures.end(), std::pair<int, Signature *>(signature->id(), signature));
        _stMem.insert(_stMem.end(), signature->id());
        ++_signaturesAdded;

        if (_vwd)
        {
            UDEBUG("%d words ref for the signature %d", signature->getWords().size(), signature->id());
        }
        if (signature->getWords().size())
        {
            signature->setEnabled(true);
        }
    }

    UDEBUG("time = %fs", timer.ticks());
}

void MemoryLoc::addSignatureToWmFromLTM(Signature *signature)
{
    if (signature)
    {
        UDEBUG("Inserting node %d in WM...", signature->id());
        _workingMem.insert(std::make_pair(signature->id(), UTimer::now()));
        _signatures.insert(std::pair<int, Signature *>(signature->id(), signature));
        ++_signaturesAdded;
    }
    else
    {
        UERROR("Signature is null ?!?");
    }
}

void MemoryLoc::moveSignatureToWMFromSTM(int id, int *reducedTo)
{
    UDEBUG("Inserting node %d from STM in WM...", id);
    UASSERT(_stMem.find(id) != _stMem.end());
    Signature *s = this->_getSignature(id);
    UASSERT(s != 0);

    if (_reduceGraph)
    {
        bool merge = false;
        const std::map<int, Link> &links = s->getLinks();
        std::map<int, Link> neighbors;
        for (std::map<int, Link>::const_iterator iter = links.begin(); iter != links.end(); ++iter)
        {
            if (!merge)
            {
                merge = iter->second.to() < s->id() && // should be a parent->child link
                        iter->second.type() != Link::kNeighbor &&
                        iter->second.type() != Link::kNeighborMerged &&
                        iter->second.userDataCompressed().empty() &&
                        iter->second.type() != Link::kUndef &&
                        iter->second.type() != Link::kVirtualClosure;
                if (merge)
                {
                    UDEBUG("Reduce %d to %d", s->id(), iter->second.to());
                    if (reducedTo)
                    {
                        *reducedTo = iter->second.to();
                    }
                }

            }
            if (iter->second.type() == Link::kNeighbor)
            {
                neighbors.insert(*iter);
            }
        }
        if (merge)
        {
            if (s->getLabel().empty())
            {
                for (std::map<int, Link>::const_iterator iter = links.begin(); iter != links.end(); ++iter)
                {
                    merge = true;
                    Signature *sTo = this->_getSignature(iter->first);
                    UASSERT(sTo != 0);
                    sTo->removeLink(s->id());
                    if (iter->second.type() != Link::kNeighbor &&
                            iter->second.type() != Link::kNeighborMerged &&
                            iter->second.type() != Link::kUndef)
                    {
                        // link to all neighbors
                        for (std::map<int, Link>::iterator jter = neighbors.begin(); jter != neighbors.end(); ++jter)
                        {
                            if (!sTo->hasLink(jter->second.to()))
                            {
                                Link l = iter->second.inverse().merge(
                                             jter->second,
                                             iter->second.userDataCompressed().empty() && iter->second.type() != Link::kVirtualClosure ? Link::kNeighborMerged : iter->second.type());
                                sTo->addLink(l);
                                Signature *sB = this->_getSignature(l.to());
                                UASSERT(sB != 0);
                                UASSERT(!sB->hasLink(l.to()));
                                sB->addLink(l.inverse());
                            }
                        }
                    }
                }

                //remove neighbor links
                std::map<int, Link> linksCopy = links;
                for (std::map<int, Link>::iterator iter = linksCopy.begin(); iter != linksCopy.end(); ++iter)
                {
                    if (iter->second.type() == Link::kNeighbor ||
                            iter->second.type() == Link::kNeighborMerged)
                    {
                        s->removeLink(iter->first);
                        if (iter->second.type() == Link::kNeighbor)
                        {
                            if (_lastGlobalLoopClosureId == s->id())
                            {
                                _lastGlobalLoopClosureId = iter->first;
                            }
                        }
                    }
                }

                this->moveToTrash(s, _notLinkedNodesKeptInDb);
                s = 0;
            }
        }
    }
    if (s != 0)
    {
        _workingMem.insert(_workingMem.end(), std::make_pair(*_stMem.begin(), UTimer::now()));
        _stMem.erase(*_stMem.begin());
    }
    // else already removed from STM/WM in moveToTrash()
}

const Signature *MemoryLoc::getSignature(int id) const
{
    return _getSignature(id);
}

Signature *MemoryLoc::_getSignature(int id) const
{
    return uValue(_signatures, id, (Signature *)0);
}

const VWDictionary *MemoryLoc::getVWDictionary() const
{
    return _vwd;
}

std::map<int, Link> MemoryLoc::getNeighborLinks(
    int signatureId,
    bool lookInDatabase) const
{
    std::map<int, Link> links;
    Signature *s = uValue(_signatures, signatureId, (Signature *)0);
    if (s)
    {
        const std::map<int, Link> &allLinks = s->getLinks();
        for (std::map<int, Link>::const_iterator iter = allLinks.begin(); iter != allLinks.end(); ++iter)
        {
            if (iter->second.type() == Link::kNeighbor ||
                    iter->second.type() == Link::kNeighborMerged)
            {
                links.insert(*iter);
            }
        }
    }
    else if (lookInDatabase && _dbDriver)
    {
        std::map<int, Link> neighbors;
        _dbDriver->loadLinks(signatureId, neighbors);
        for (std::map<int, Link>::iterator iter = neighbors.begin(); iter != neighbors.end();)
        {
            if (iter->second.type() != Link::kNeighbor &&
                    iter->second.type() != Link::kNeighborMerged)
            {
                neighbors.erase(iter++);
            }
            else
            {
                ++iter;
            }
        }
    }
    else
    {
        UWARN("Cannot find signature %d in memory", signatureId);
    }
    return links;
}

std::map<int, Link> MemoryLoc::getLinks(
    int signatureId,
    bool lookInDatabase) const
{
    std::map<int, Link> links;
    Signature *s = uValue(_signatures, signatureId, (Signature *)0);
    if (s)
    {
        links = s->getLinks();
    }
    else if (lookInDatabase && _dbDriver)
    {
        _dbDriver->loadLinks(signatureId, links, Link::kUndef);
    }
    else
    {
        UWARN("Cannot find signature %d in memory", signatureId);
    }
    return links;
}

// return map<Id,Margin>, including signatureId
// maxCheckedInDatabase = -1 means no limit to check in database (default)
// maxCheckedInDatabase = 0 means don't check in database
std::map<int, int> MemoryLoc::getNeighborsId(
    int signatureId,
    int maxGraphDepth, // 0 means infinite margin
    int maxCheckedInDatabase, // default -1 (no limit)
    bool incrementMarginOnLoop, // default false
    bool ignoreLoopIds, // default false
    bool ignoreIntermediateNodes, // default false
    double *dbAccessTime
) const
{
    UASSERT(maxGraphDepth >= 0);
    //UDEBUG("signatureId=%d, neighborsMargin=%d", signatureId, margin);
    if (dbAccessTime)
    {
        *dbAccessTime = 0;
    }
    std::map<int, int> ids;
    if (signatureId <= 0)
    {
        return ids;
    }
    int nbLoadedFromDb = 0;
    std::list<int> curentMarginList;
    std::set<int> currentMargin;
    std::set<int> nextMargin;
    nextMargin.insert(signatureId);
    int m = 0;
    std::set<int> ignoredIds;
    while ((maxGraphDepth == 0 || m < maxGraphDepth) && nextMargin.size())
    {
        // insert more recent first (priority to be loaded first from the database below if set)
        curentMarginList = std::list<int>(nextMargin.rbegin(), nextMargin.rend());
        nextMargin.clear();

        for (std::list<int>::iterator jter = curentMarginList.begin(); jter != curentMarginList.end(); ++jter)
        {
            if (ids.find(*jter) == ids.end())
            {
                //UDEBUG("Added %d with margin %d", *jter, m);
                // Look up in STM/WM if all ids are here, if not... load them from the database
                const Signature *s = this->getSignature(*jter);
                std::map<int, Link> tmpLinks;
                const std::map<int, Link> *links = &tmpLinks;
                if (s)
                {
                    if (!ignoreIntermediateNodes || s->getWeight() != -1)
                    {
                        ids.insert(std::pair<int, int>(*jter, m));
                    }
                    else
                    {
                        ignoredIds.insert(*jter);
                    }

                    links = &s->getLinks();
                }
                else if (maxCheckedInDatabase == -1 || (maxCheckedInDatabase > 0 && _dbDriver && nbLoadedFromDb < maxCheckedInDatabase))
                {
                    ++nbLoadedFromDb;
                    ids.insert(std::pair<int, int>(*jter, m));

                    UTimer timer;
                    _dbDriver->loadLinks(*jter, tmpLinks);
                    if (dbAccessTime)
                    {
                        *dbAccessTime += timer.getElapsedTime();
                    }
                }

                // links
                for (std::map<int, Link>::const_iterator iter = links->begin(); iter != links->end(); ++iter)
                {
                    if (!uContains(ids, iter->first) && ignoredIds.find(iter->first) == ignoredIds.end())
                    {
                        UASSERT(iter->second.type() != Link::kUndef);
                        if (iter->second.type() == Link::kNeighbor ||
                                iter->second.type() == Link::kNeighborMerged)
                        {
                            if (ignoreIntermediateNodes && s->getWeight() == -1)
                            {
                                // stay on the same margin
                                if (currentMargin.insert(iter->first).second)
                                {
                                    curentMarginList.push_back(iter->first);
                                }
                            }
                            else
                            {
                                nextMargin.insert(iter->first);
                            }
                        }
                        else if (!ignoreLoopIds)
                        {
                            if (incrementMarginOnLoop)
                            {
                                nextMargin.insert(iter->first);
                            }
                            else
                            {
                                if (currentMargin.insert(iter->first).second)
                                {
                                    curentMarginList.push_back(iter->first);
                                }
                            }
                        }
                    }
                }
            }
        }
        ++m;
    }
    return ids;
}

// return map<Id,sqrdDistance>, including signatureId
std::map<int, float> MemoryLoc::getNeighborsIdRadius(
    int signatureId,
    float radius, // 0 means ignore radius
    const std::map<int, Transform> &optimizedPoses,
    int maxGraphDepth // 0 means infinite margin
) const
{
    UASSERT(maxGraphDepth >= 0);
    UASSERT(uContains(optimizedPoses, signatureId));
    UASSERT(signatureId > 0);
    std::map<int, float> ids;
    std::list<int> curentMarginList;
    std::set<int> currentMargin;
    std::set<int> nextMargin;
    nextMargin.insert(signatureId);
    int m = 0;
    Transform referential = optimizedPoses.at(signatureId);
    UASSERT(!referential.isNull());
    float radiusSqrd = radius * radius;
    std::map<int, float> savedRadius;
    savedRadius.insert(std::make_pair(signatureId, 0));
    while ((maxGraphDepth == 0 || m < maxGraphDepth) && nextMargin.size())
    {
        curentMarginList = std::list<int>(nextMargin.begin(), nextMargin.end());
        nextMargin.clear();

        for (std::list<int>::iterator jter = curentMarginList.begin(); jter != curentMarginList.end(); ++jter)
        {
            if (ids.find(*jter) == ids.end())
            {
                //UDEBUG("Added %d with margin %d", *jter, m);
                // Look up in STM/WM if all ids are here, if not... load them from the database
                const Signature *s = this->getSignature(*jter);
                std::map<int, Link> tmpLinks;
                const std::map<int, Link> *links = &tmpLinks;
                if (s)
                {
                    ids.insert(std::pair<int, float>(*jter, savedRadius.at(*jter)));

                    links = &s->getLinks();
                }

                // links
                for (std::map<int, Link>::const_iterator iter = links->begin(); iter != links->end(); ++iter)
                {
                    if (!uContains(ids, iter->first) &&
                            uContains(optimizedPoses, iter->first) &&
                            iter->second.type() != Link::kVirtualClosure)
                    {
                        const Transform &t = optimizedPoses.at(iter->first);
                        UASSERT(!t.isNull());
                        float distanceSqrd = referential.getDistanceSquared(t);
                        if (radiusSqrd == 0 || distanceSqrd < radiusSqrd)
                        {
                            savedRadius.insert(std::make_pair(iter->first, distanceSqrd));
                            nextMargin.insert(iter->first);
                        }

                    }
                }
            }
        }
        ++m;
    }
    return ids;
}

int MemoryLoc::getNextId()
{
    return ++_idCount;
}

int MemoryLoc::incrementMapId(std::map<int, int> *reducedIds)
{
    //don't increment if there is no location in the current map
    const Signature *s = getLastWorkingSignature();
    if (s && s->mapId() == _idMapCount)
    {
        // New session! move all signatures from the STM to WM
        while (_stMem.size())
        {
            int reducedId = 0;
            int id = *_stMem.begin();
            moveSignatureToWMFromSTM(id, &reducedId);
            if (reducedIds && reducedId > 0)
            {
                reducedIds->insert(std::make_pair(id, reducedId));
            }
        }

        return ++_idMapCount;
    }
    return _idMapCount;
}

std::set<int> MemoryLoc::getAllSignatureIds() const
{
    std::set<int> ids;
    if (_dbDriver)
    {
        _dbDriver->getAllNodeIds(ids);
    }
    for (std::map<int, Signature *>::const_iterator iter = _signatures.begin(); iter != _signatures.end(); ++iter)
    {
        ids.insert(iter->first);
    }
    return ids;
}

void MemoryLoc::clear()
{
    UDEBUG("");

    // empty the STM
    while (_stMem.size())
    {
        moveSignatureToWMFromSTM(*_stMem.begin());
    }
    if (_stMem.size() != 0)
    {
        ULOGGER_ERROR("_stMem must be empty here, size=%d", _stMem.size());
    }
    _stMem.clear();

    this->cleanUnusedWords();

    if (_dbDriver)
    {
        _dbDriver->emptyTrashes();
        _dbDriver->join();
    }
    if (_dbDriver)
    {
        // make sure time_enter in database is at least 1 second
        // after for the next stuf added to database
        uSleep(1500);
    }

    // Save some stats to the db, save only when the mem is not empty
    if (_dbDriver && (_stMem.size() || _workingMem.size()))
    {
        unsigned int memSize = (unsigned int)(_workingMem.size() + _stMem.size());
        if (_workingMem.size() && _workingMem.begin()->first < 0)
        {
            --memSize;
        }

        // this is only a safe check...not supposed to occur.
        UASSERT_MSG(memSize == _signatures.size(),
                    uFormat("The number of signatures don't match! _workingMem=%d, _stMem=%d, _signatures=%d",
                            _workingMem.size(), _stMem.size(), _signatures.size()).c_str());

        UDEBUG("Adding statistics after run...");
        if (_memoryChanged)
        {
            UDEBUG("");
            _dbDriver->addStatisticsAfterRun(memSize,
                                             _lastSignature ? _lastSignature->id() : 0,
                                             UProcessInfo::getMemoryUsage(),
                                             _dbDriver->getMemoryUsed(),
                                             (int)_vwd->getVisualWords().size());
        }
    }
    UDEBUG("");

    //Get the tree root (parents)
    std::map<int, Signature *> mem = _signatures;
    for (std::map<int, Signature *>::iterator i = mem.begin(); i != mem.end(); ++i)
    {
        if (i->second)
        {
            UDEBUG("deleting from the working and the short-term memory: %d", i->first);
            this->moveToTrash(i->second);
        }
    }

    if (_workingMem.size() != 0 && !(_workingMem.size() == 1 && _workingMem.begin()->first == kIdVirtual))
    {
        ULOGGER_ERROR("_workingMem must be empty here, size=%d", _workingMem.size());
    }
    _workingMem.clear();
    if (_signatures.size() != 0)
    {
        ULOGGER_ERROR("_signatures must be empty here, size=%d", _signatures.size());
    }
    _signatures.clear();

    UDEBUG("");
    // Wait until the db trash has finished cleaning the memory
    if (_dbDriver)
    {
        _dbDriver->emptyTrashes();
    }
    UDEBUG("");
    _lastSignature = 0;
    _lastGlobalLoopClosureId = 0;
    _idCount = kIdStart;
    _idMapCount = kIdStart;
    _memoryChanged = false;
    _linksChanged = false;

    if (_dbDriver)
    {
        _dbDriver->join(true);
        cleanUnusedWords();
        _dbDriver->emptyTrashes();
    }
    else
    {
        cleanUnusedWords();
    }
    if (_vwd)
    {
        _vwd->clear();
    }
    UDEBUG("");
}

/**
 * Compute the likelihood of the signature with some others in the memory.
 * Important: Assuming that all other ids are under 'signature' id.
 * If an error occurs, the result is empty.
 */
std::map<int, float> MemoryLoc::computeLikelihood(const Signature *signature, const std::list<int> &ids)
{
    if (!_tfIdfLikelihoodUsed)
    {
        UTimer timer;
        timer.start();
        std::map<int, float> likelihood;

        if (!signature)
        {
            ULOGGER_ERROR("The signature is null");
            return likelihood;
        }
        else if (ids.empty())
        {
            UWARN("ids list is empty");
            return likelihood;
        }

        for (std::list<int>::const_iterator iter = ids.begin(); iter != ids.end(); ++iter)
        {
            float sim = 0.0f;
            if (*iter > 0)
            {
                const Signature *sB = this->getSignature(*iter);
                if (!sB)
                {
                    UFATAL("Signature %d not found in WM ?!?", *iter);
                }
                sim = signature->compareTo(*sB);
            }

            likelihood.insert(likelihood.end(), std::pair<int, float>(*iter, sim));
        }

        UDEBUG("compute likelihood (similarity)... %f s", timer.ticks());
        return likelihood;
    }
    else
    {
        UTimer timer;
        timer.start();
        std::map<int, float> likelihood;
        std::map<int, float> calculatedWordsRatio;

        if (!signature)
        {
            ULOGGER_ERROR("The signature is null");
            return likelihood;
        }
        else if (ids.empty())
        {
            UWARN("ids list is empty");
            return likelihood;
        }

        for (std::list<int>::const_iterator iter = ids.begin(); iter != ids.end(); ++iter)
        {
            likelihood.insert(likelihood.end(), std::pair<int, float>(*iter, 0.0f));
        }

        const std::list<int> &wordIds = uUniqueKeys(signature->getWords());

        float nwi; // nwi is the number of a specific word referenced by a place
        float ni; // ni is the total of words referenced by a place
        float nw; // nw is the number of places referenced by a specific word
        float N; // N is the total number of places

        float logNnw;
        const VisualWord *vw;

        N = this->getSignatures().size();

        if (N)
        {
            UDEBUG("processing... ");
            // Pour chaque mot dans la signature SURF
            for (std::list<int>::const_iterator i = wordIds.begin(); i != wordIds.end(); ++i)
            {
                // "Inverted index" - Pour chaque endroit contenu dans chaque mot
                vw = _vwd->getWord(*i);
                if (vw)
                {
                    const std::map<int, int> &refs = vw->getReferences();
                    nw = refs.size();
                    if (nw)
                    {
                        logNnw = log10(N / nw);
                        if (logNnw)
                        {
                            for (std::map<int, int>::const_iterator j = refs.begin(); j != refs.end(); ++j)
                            {
                                std::map<int, float>::iterator iter = likelihood.find(j->first);
                                if (iter != likelihood.end())
                                {
                                    nwi = j->second;
                                    ni = this->getNi(j->first);
                                    if (ni != 0)
                                    {
                                        //UDEBUG("%d, %f %f %f %f", vw->id(), logNnw, nwi, ni, ( nwi  * logNnw ) / ni);
                                        iter->second += (nwi  * logNnw) / ni;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        UDEBUG("compute likelihood (tf-idf) %f s", timer.ticks());
        return likelihood;
    }
}

rtabmap::Transform MemoryLoc::computeGlobalVisualTransform(const std::vector<int> &oldIds, int newId) const
{
    std::vector<const rtabmap::Signature *> oldSigs;
    for (std::vector<int>::const_iterator it = oldIds.begin() ; it != oldIds.end(); it++)
    {
        const rtabmap::Signature *oldSig = getSignature(*it);
        if (oldSig == NULL)
        {
            return rtabmap::Transform();
        }
        oldSigs.push_back(oldSig);
    }

    const rtabmap::Signature *newSig = getSignature(newId);
    if (newSig == NULL)
    {
        return rtabmap::Transform();
    }

    return computeGlobalVisualTransform(oldSigs, newSig);
}

rtabmap::Transform MemoryLoc::computeGlobalVisualTransform(const std::vector<const rtabmap::Signature *> &oldSigs, const rtabmap::Signature *newSig) const
{
    if (oldSigs.size() == 0 || newSig == NULL)
    {
        return rtabmap::Transform();
    }

    rtabmap::Transform transform;
    std::string msg;

    int inliersCount = 0;
    double variance = 1.0;

    std::multimap<int, cv::Point3f> words3;

    const std::vector<const rtabmap::Signature *>::const_iterator firstSig = oldSigs.begin();
    const rtabmap::Transform &guessPose = (*firstSig)->getPose();

    for (std::vector<const rtabmap::Signature *>::const_iterator sigIter = oldSigs.begin(); sigIter != oldSigs.end(); sigIter++)
    {
        rtabmap::Transform pose = (*sigIter)->getPose();
        const std::multimap<int, cv::Point3f> &sigWords3 = (*sigIter)->getWords3();
        std::multimap<int, cv::Point3f>::const_iterator word3Iter;
        for (word3Iter = sigWords3.begin(); word3Iter != sigWords3.end(); word3Iter++)
        {
            cv::Point3f point3 = rtabmap::util3d::transformPoint(word3Iter->second, pose);
            words3.insert(std::pair<int, cv::Point3f>(word3Iter->first, point3));
        }
    }

    // 3D to 2D (PnP)
    if ((int)words3.size() >= _minInliers && (int)newSig->getWords().size() >= _minInliers)
    {
        const rtabmap::CameraModel &cameraModel = newSig->sensorData().cameraModels()[0];

        std::vector<int> matches;
        std::vector<int> inliers;
        transform = rtabmap::util3d::estimateMotion3DTo2D(
                        uMultimapToMapUnique(words3),
                        uMultimapToMapUnique(newSig->getWords()),
                        cameraModel, // TODO: cameraModel.localTransform has to be the same for all images
                        _minInliers,
                        _iterations,
                        _pnpReprojError,
                        _pnpFlags,
                        _pnpRefineIterations,
                        guessPose, // use the first signature's pose as a guess
                        uMultimapToMapUnique(newSig->getWords3()),
                        &variance,
                        &matches,
                        &inliers);
        inliersCount = (int)inliers.size();
        if (transform.isNull())
        {
            msg = uFormat("Not enough inliers %d/%d between the old signatures and %d", inliersCount, _minInliers, newSig->id());
            UINFO(msg.c_str());
        }
    }
    else
    {
        msg = uFormat("Not enough features in images (old=%d, new=%d, min=%d)", (int)words3.size(), (int)newSig->getWords().size(), _minInliers);
        UINFO(msg.c_str());
    }

    // TODO check RegistrationVis.cpp to see whether this is necessary
    if (!transform.isNull())
    {
        // verify if it is a 180 degree transform, well verify > 90
        float x, y, z, roll, pitch, yaw;
        transform.inverse().getTranslationAndEulerAngles(x, y, z, roll, pitch, yaw);
        if (fabs(roll) > CV_PI / 2 || fabs(pitch) > CV_PI / 2 || fabs(yaw) > CV_PI / 2)
        {
            transform.setNull();
            msg = uFormat("Too large rotation detected! (roll=%f, pitch=%f, yaw=%f)", roll, pitch, yaw);
            UWARN(msg.c_str());
        }
    }

    UDEBUG("transform=%s", transform.prettyPrint().c_str());
    return transform;
}

int MemoryLoc::cleanup()
{
    UDEBUG("");
    int signatureRemoved = 0;

    // bad signature
    if (_lastSignature && ((_lastSignature->isBadSignature() && _badSignaturesIgnored) || !_incrementalMemory))
    {
        if (_lastSignature->isBadSignature())
        {
            UDEBUG("Bad signature! %d", _lastSignature->id());
        }
        signatureRemoved = _lastSignature->id();
        moveToTrash(_lastSignature, _incrementalMemory);
    }

    return signatureRemoved;
}

void MemoryLoc::emptyTrash()
{
    if (_dbDriver)
    {
        _dbDriver->emptyTrashes(true);
    }
}

void MemoryLoc::joinTrashThread()
{
    if (_dbDriver)
    {
        UDEBUG("");
        _dbDriver->join();
        UDEBUG("");
    }
}

class WeightAgeIdKey
{
public:
    WeightAgeIdKey(int w, double a, int i) :
        weight(w),
        age(a),
        id(i) {}
    bool operator<(const WeightAgeIdKey &k) const
    {
        if (weight < k.weight)
        {
            return true;
        }
        else if (weight == k.weight)
        {
            if (age < k.age)
            {
                return true;
            }
            else if (age == k.age)
            {
                if (id < k.id)
                {
                    return true;
                }
            }
        }
        return false;
    }
    int weight, age, id;
};
std::list<Signature *> MemoryLoc::getRemovableSignatures(int count, const std::set<int> &ignoredIds)
{
    //UDEBUG("");
    std::list<Signature *> removableSignatures;
    std::map<WeightAgeIdKey, Signature *> weightAgeIdMap;

    // Find the last index to check...
    UDEBUG("mem.size()=%d, ignoredIds.size()=%d", (int)_workingMem.size(), (int)ignoredIds.size());

    if (_workingMem.size())
    {
        int recentWmMaxSize = _recentWmRatio * float(_workingMem.size());
        bool recentWmImmunized = false;
        // look for the position of the lastLoopClosureId in WM
        int currentRecentWmSize = 0;
        if (_lastGlobalLoopClosureId > 0 && _stMem.find(_lastGlobalLoopClosureId) == _stMem.end())
        {
            // If set, it must be in WM
            std::map<int, double>::const_iterator iter = _workingMem.find(_lastGlobalLoopClosureId);
            while (iter != _workingMem.end())
            {
                ++currentRecentWmSize;
                ++iter;
            }
            if (currentRecentWmSize > 1 && currentRecentWmSize < recentWmMaxSize)
            {
                recentWmImmunized = true;
            }
            else if (currentRecentWmSize == 0 && _workingMem.size() > 1)
            {
                UERROR("Last loop closure id not found in WM (%d)", _lastGlobalLoopClosureId);
            }
            UDEBUG("currentRecentWmSize=%d, recentWmMaxSize=%d, _recentWmRatio=%f, end recent wM = %d", currentRecentWmSize, recentWmMaxSize, _recentWmRatio, _lastGlobalLoopClosureId);
        }

        // Ignore neighbor of the last location in STM (for neighbor links redirection issue during Rehearsal).
        Signature *lastInSTM = 0;
        if (_stMem.size())
        {
            lastInSTM = _signatures.at(*_stMem.begin());
        }

        for (std::map<int, double>::const_iterator memIter = _workingMem.begin(); memIter != _workingMem.end(); ++memIter)
        {
            if ((recentWmImmunized && memIter->first > _lastGlobalLoopClosureId) ||
                    memIter->first == _lastGlobalLoopClosureId)
            {
                // ignore recent memory
            }
            else if (memIter->first > 0 && ignoredIds.find(memIter->first) == ignoredIds.end() && (!lastInSTM || !lastInSTM->hasLink(memIter->first)))
            {
                Signature *s = this->_getSignature(memIter->first);
                if (s)
                {
                    // Links must not be in STM to be removable, rehearsal issue
                    bool foundInSTM = false;
                    for (std::map<int, Link>::const_iterator iter = s->getLinks().begin(); iter != s->getLinks().end(); ++iter)
                    {
                        if (_stMem.find(iter->first) != _stMem.end())
                        {
                            UDEBUG("Ignored %d because it has a link (%d) to STM", s->id(), iter->first);
                            foundInSTM = true;
                            break;
                        }
                    }
                    if (!foundInSTM)
                    {
                        // less weighted signature priority to be transferred
                        weightAgeIdMap.insert(std::make_pair(WeightAgeIdKey(s->getWeight(), _transferSortingByWeightId ? 0.0 : memIter->second, s->id()), s));
                    }
                }
                else
                {
                    ULOGGER_ERROR("Not supposed to occur!!!");
                }
            }
            else
            {
                //UDEBUG("Ignoring id %d", memIter->first);
            }
        }

        int recentWmCount = 0;
        // make the list of removable signatures
        // Criteria : Weight -> ID
        UDEBUG("signatureMap.size()=%d _lastGlobalLoopClosureId=%d currentRecentWmSize=%d recentWmMaxSize=%d",
               (int)weightAgeIdMap.size(), _lastGlobalLoopClosureId, currentRecentWmSize, recentWmMaxSize);
        for (std::map<WeightAgeIdKey, Signature *>::iterator iter = weightAgeIdMap.begin();
                iter != weightAgeIdMap.end();
                ++iter)
        {
            if (!recentWmImmunized)
            {
                UDEBUG("weight=%d, id=%d",
                       iter->second->getWeight(),
                       iter->second->id());
                removableSignatures.push_back(iter->second);

                if (_lastGlobalLoopClosureId && iter->second->id() > _lastGlobalLoopClosureId)
                {
                    ++recentWmCount;
                    if (currentRecentWmSize - recentWmCount < recentWmMaxSize)
                    {
                        UDEBUG("switched recentWmImmunized");
                        recentWmImmunized = true;
                    }
                }
            }
            else if (_lastGlobalLoopClosureId == 0 || iter->second->id() < _lastGlobalLoopClosureId)
            {
                UDEBUG("weight=%d, id=%d",
                       iter->second->getWeight(),
                       iter->second->id());
                removableSignatures.push_back(iter->second);
            }
            if (removableSignatures.size() >= (unsigned int)count)
            {
                break;
            }
        }
    }
    else
    {
        ULOGGER_WARN("not enough signatures to get an old one...");
    }
    return removableSignatures;
}

/**
 * If saveToDatabase=false, deleted words are filled in deletedWords.
 */
void MemoryLoc::moveToTrash(Signature *s, bool keepLinkedToGraph, std::list<int> *deletedWords)
{
    UDEBUG("id=%d", s ? s->id() : 0);
    if (s)
    {
        // If not saved to database or it is a bad signature (not saved), remove links!
        if (!keepLinkedToGraph || (!s->isSaved() && s->isBadSignature() && _badSignaturesIgnored))
        {
            UASSERT_MSG(this->isInSTM(s->id()),
                        uFormat("Deleting location (%d) outside the "
                                "STM is not implemented!", s->id()).c_str());
            const std::map<int, Link> &links = s->getLinks();
            for (std::map<int, Link>::const_iterator iter = links.begin(); iter != links.end(); ++iter)
            {
                Signature *sTo = this->_getSignature(iter->first);
                // neighbor to s
                UASSERT_MSG(sTo != 0,
                            uFormat("A neighbor (%d) of the deleted location %d is "
                                    "not found in WM/STM! Are you deleting a location "
                                    "outside the STM?", iter->first, s->id()).c_str());

                if (iter->first > s->id() && links.size() > 1 && sTo->hasLink(s->id()))
                {
                    UWARN("Link %d of %d is newer, removing neighbor link "
                          "may split the map!",
                          iter->first, s->id());
                }

                // child
                if (iter->second.type() == Link::kGlobalClosure && s->id() > sTo->id())
                {
                    sTo->setWeight(sTo->getWeight() + s->getWeight()); // copy weight
                }

                sTo->removeLink(s->id());

            }
            s->removeLinks(); // remove all links
            s->setWeight(0);
            s->setLabel(""); // reset label
        }
        else
        {
            // Make sure that virtual links are removed.
            // It should be called before the signature is
            // removed from _signatures below.
            removeVirtualLinks(s->id());
        }

        this->disableWordsRef(s->id());
        if (!keepLinkedToGraph)
        {
            std::list<int> keys = uUniqueKeys(s->getWords());
            for (std::list<int>::const_iterator i = keys.begin(); i != keys.end(); ++i)
            {
                // assume just removed word doesn't have any other references
                VisualWord *w = _vwd->getUnusedWord(*i);
                if (w)
                {
                    std::vector<VisualWord *> wordToDelete;
                    wordToDelete.push_back(w);
                    _vwd->removeWords(wordToDelete);
                    if (deletedWords)
                    {
                        deletedWords->push_back(w->id());
                    }
                    delete w;
                }
            }
        }

        _workingMem.erase(s->id());
        _stMem.erase(s->id());
        _signatures.erase(s->id());
        if (_signaturesAdded > 0)
        {
            --_signaturesAdded;
        }

        if (_lastSignature == s)
        {
            _lastSignature = 0;
            if (_stMem.size())
            {
                _lastSignature = this->_getSignature(*_stMem.rbegin());
            }
            else if (_workingMem.size())
            {
                _lastSignature = this->_getSignature(_workingMem.rbegin()->first);
            }
        }

        if (_lastGlobalLoopClosureId == s->id())
        {
            _lastGlobalLoopClosureId = 0;
        }

        if ((_notLinkedNodesKeptInDb || keepLinkedToGraph) &&
                _dbDriver &&
                s->id() > 0 &&
                (_incrementalMemory || s->isSaved()))
        {
            _dbDriver->asyncSave(s);
        }
        else
        {
            delete s;
        }
    }
}

const Signature *MemoryLoc::getLastWorkingSignature() const
{
    UDEBUG("");
    return _lastSignature;
}

int MemoryLoc::getSignatureIdByLabel(const std::string &label, bool lookInDatabase) const
{
    UDEBUG("label=%s", label.c_str());
    int id = 0;
    if (label.size())
    {
        for (std::map<int, Signature *>::const_iterator iter = _signatures.begin(); iter != _signatures.end(); ++iter)
        {
            UASSERT(iter->second != 0);
            if (iter->second->getLabel().compare(label) == 0)
            {
                id = iter->second->id();
                break;
            }
        }
        if (id == 0 && _dbDriver && lookInDatabase)
        {
            _dbDriver->getNodeIdByLabel(label, id);
        }
    }
    return id;
}

std::map<int, std::string> MemoryLoc::getAllLabels() const
{
    std::map<int, std::string> labels;
    for (std::map<int, Signature *>::const_iterator iter = _signatures.begin(); iter != _signatures.end(); ++iter)
    {
        if (!iter->second->getLabel().empty())
        {
            labels.insert(std::make_pair(iter->first, iter->second->getLabel()));
        }
    }
    if (_dbDriver)
    {
        _dbDriver->getAllLabels(labels);
    }
    return labels;
}

void Memory::deleteLocation(int locationId, std::list<int> *deletedWords)
{
    UDEBUG("Deleting location %d", locationId);
    Signature *location = _getSignature(locationId);
    if (location)
    {
        this->moveToTrash(location, false, deletedWords);
    }
}

void MemoryLoc::removeLink(int oldId, int newId)
{
    //this method assumes receiving oldId < newId, if not switch them
    Signature *oldS = this->_getSignature(oldId < newId ? oldId : newId);
    Signature *newS = this->_getSignature(oldId < newId ? newId : oldId);
    if (oldS && newS)
    {
        UINFO("removing link between location %d and %d", oldS->id(), newS->id());

        if (oldS->hasLink(newS->id()) && newS->hasLink(oldS->id()))
        {
            Link::Type type = oldS->getLinks().at(newS->id()).type();
            if (type == Link::kGlobalClosure && newS->getWeight() > 0)
            {
                // adjust the weight
                oldS->setWeight(oldS->getWeight() + 1);
                newS->setWeight(newS->getWeight() > 0 ? newS->getWeight() - 1 : 0);
            }


            oldS->removeLink(newS->id());
            newS->removeLink(oldS->id());

            if (type != Link::kVirtualClosure)
            {
                _linksChanged = true;
            }

            bool noChildrenAnymore = true;
            for (std::map<int, Link>::const_iterator iter = newS->getLinks().begin(); iter != newS->getLinks().end(); ++iter)
            {
                if (iter->second.type() != Link::kNeighbor &&
                        iter->second.type() != Link::kNeighborMerged &&
                        iter->first < newS->id())
                {
                    noChildrenAnymore = false;
                    break;
                }
            }
            if (noChildrenAnymore && newS->id() == _lastGlobalLoopClosureId)
            {
                _lastGlobalLoopClosureId = 0;
            }
        }
        else
        {
            UERROR("Signatures %d and %d don't have bidirectional link!", oldS->id(), newS->id());
        }
    }
    else
    {
        if (!newS)
        {
            UERROR("Signature %d is not in working memory... cannot remove link.", newS->id());
        }
        if (!oldS)
        {
            UERROR("Signature %d is not in working memory... cannot remove link.", oldS->id());
        }
    }
}

bool MemoryLoc::addLink(const Link &link)
{
    UASSERT(link.type() > Link::kNeighbor && link.type() != Link::kUndef);

    ULOGGER_INFO("to=%d, from=%d transform: %s var=%f", link.to(), link.from(), link.transform().prettyPrint().c_str(), link.transVariance());
    Signature *toS = _getSignature(link.to());
    Signature *fromS = _getSignature(link.from());
    if (toS && fromS)
    {
        if (toS->hasLink(link.from()))
        {
            // do nothing, already merged
            UINFO("already linked! to=%d, from=%d", link.to(), link.from());
            return true;
        }

        UDEBUG("Add link between %d and %d", toS->id(), fromS->id());

        toS->addLink(link.inverse());
        fromS->addLink(link);

        if (_incrementalMemory)
        {
            if (link.type() != Link::kVirtualClosure)
            {
                _linksChanged = true;

                // update weight
                // ignore scan matching loop closures
                if (link.type() != Link::kLocalSpaceClosure ||
                        link.userDataCompressed().empty())
                {
                    _lastGlobalLoopClosureId = fromS->id() > toS->id() ? fromS->id() : toS->id();

                    // update weights only if the memory is incremental
                    // When reducing the graph, transfer weight to the oldest signature
                    UASSERT(fromS->getWeight() >= 0 && toS->getWeight() >= 0);
                    if ((_reduceGraph && fromS->id() < toS->id()) ||
                            (!_reduceGraph && fromS->id() > toS->id()))
                    {
                        fromS->setWeight(fromS->getWeight() + toS->getWeight());
                        toS->setWeight(0);
                    }
                    else
                    {
                        toS->setWeight(toS->getWeight() + fromS->getWeight());
                        fromS->setWeight(0);
                    }
                }
            }
        }
        return true;
    }
    else
    {
        if (!fromS)
        {
            UERROR("from=%d, to=%d, Signature %d not found in working/st memories", link.from(), link.to(), link.from());
        }
        if (!toS)
        {
            UERROR("from=%d, to=%d, Signature %d not found in working/st memories", link.from(), link.to(), link.to());
        }
    }
    return false;
}

void MemoryLoc::removeVirtualLinks(int signatureId)
{
    UDEBUG("");
    Signature *s = this->_getSignature(signatureId);
    if (s)
    {
        const std::map<int, Link> &links = s->getLinks();
        for (std::map<int, Link>::const_iterator iter = links.begin(); iter != links.end(); ++iter)
        {
            if (iter->second.type() == Link::kVirtualClosure)
            {
                Signature *sTo = this->_getSignature(iter->first);
                if (sTo)
                {
                    sTo->removeLink(s->id());
                }
                else
                {
                    UERROR("Link %d of %d not in WM/STM?!?", iter->first, s->id());
                }
            }
        }
        s->removeVirtualLinks();
    }
    else
    {
        UERROR("Signature %d not in WM/STM?!?", signatureId);
    }
}

void MemoryLoc::rehearsal(Signature *signature, Statistics *stats)
{
    UTimer timer;
    if (signature->getLinks().size() != 1 ||
            signature->isBadSignature())
    {
        return;
    }

    //============================================================
    // Compare with the last (not intermediate node)
    //============================================================
    Signature *sB = 0;
    for (std::set<int>::reverse_iterator iter = _stMem.rbegin(); iter != _stMem.rend(); ++iter)
    {
        Signature *s = this->_getSignature(*iter);
        UASSERT(s != 0);
        if (s->getWeight() >= 0 && s->id() != signature->id())
        {
            sB = s;
            break;
        }
    }
    if (sB)
    {
        int id = sB->id();
        UDEBUG("Comparing with signature (%d)...", id);

        float sim = signature->compareTo(*sB);

        int merged = 0;
        if (sim >= _similarityThreshold)
        {
            if (_incrementalMemory)
            {
                if (this->rehearsalMerge(id, signature->id()))
                {
                    merged = id;
                }
            }
            else
            {
                signature->setWeight(signature->getWeight() + 1 + sB->getWeight());
            }
        }

        if (stats) stats->addStatistic(Statistics::kMemoryRehearsal_merged(), merged);
        if (stats) stats->addStatistic(Statistics::kMemoryRehearsal_sim(), sim);
        if (stats) stats->addStatistic(Statistics::kMemoryRehearsal_id(), sim >= _similarityThreshold ? id : 0);
        UDEBUG("merged=%d, sim=%f t=%fs", merged, sim, timer.ticks());
    }
    else
    {
        if (stats) stats->addStatistic(Statistics::kMemoryRehearsal_merged(), 0);
        if (stats) stats->addStatistic(Statistics::kMemoryRehearsal_sim(), 0);
    }
}

bool MemoryLoc::rehearsalMerge(int oldId, int newId)
{
    ULOGGER_INFO("old=%d, new=%d", oldId, newId);
    Signature *oldS = _getSignature(oldId);
    Signature *newS = _getSignature(newId);
    if (oldS && newS && _incrementalMemory)
    {
        UASSERT_MSG(oldS->getWeight() >= 0 && newS->getWeight() >= 0, uFormat("%d %d", oldS->getWeight(), newS->getWeight()).c_str());
        std::map<int, Link>::const_iterator iter = oldS->getLinks().find(newS->id());
        if (iter != oldS->getLinks().end() &&
                iter->second.type() != Link::kNeighbor &&
                iter->second.type() != Link::kNeighborMerged)
        {
            // do nothing, already merged
            UWARN("already merged, old=%d, new=%d", oldId, newId);
            return false;
        }
        UASSERT(!newS->isSaved());

        UINFO("Rehearsal merging %d (w=%d) and %d (w=%d)",
              oldS->id(), oldS->getWeight(),
              newS->id(), newS->getWeight());

        bool fullMerge;
        bool intermediateMerge = false;
        if (!newS->getLinks().begin()->second.transform().isNull())
        {
            // we are in metric SLAM mode:
            // 1) Normal merge if not moving AND has direct link
            // 2) Transform to intermediate node (weight = -1) if not moving AND hasn't direct link.
            float x, y, z, roll, pitch, yaw;
            newS->getLinks().begin()->second.transform().getTranslationAndEulerAngles(x, y, z, roll, pitch, yaw);
            bool isMoving = fabs(x) > _rehearsalMaxDistance ||
                            fabs(y) > _rehearsalMaxDistance ||
                            fabs(z) > _rehearsalMaxDistance ||
                            fabs(roll) > _rehearsalMaxAngle ||
                            fabs(pitch) > _rehearsalMaxAngle ||
                            fabs(yaw) > _rehearsalMaxAngle;
            if (isMoving && _rehearsalWeightIgnoredWhileMoving)
            {
                UINFO("Rehearsal ignored because the robot has moved more than %f m or %f rad (\"Mem/RehearsalWeightIgnoredWhileMoving\"=true)",
                      _rehearsalMaxDistance, _rehearsalMaxAngle);
                return false;
            }
            fullMerge = !isMoving && newS->hasLink(oldS->id());
            intermediateMerge = !isMoving && !newS->hasLink(oldS->id());
        }
        else
        {
            fullMerge = newS->hasLink(oldS->id()) && newS->getLinks().begin()->second.transform().isNull();
        }

        if (fullMerge)
        {
            //remove mutual links
            Link newToOldLink = newS->getLinks().at(oldS->id());
            oldS->removeLink(newId);
            newS->removeLink(oldId);

            if (_idUpdatedToNewOneRehearsal)
            {
                // redirect neighbor links
                const std::map<int, Link> &links = oldS->getLinks();
                for (std::map<int, Link>::const_iterator iter = links.begin(); iter != links.end(); ++iter)
                {
                    Link link = iter->second;
                    Link mergedLink = newToOldLink.merge(link, link.type());
                    UASSERT(mergedLink.from() == newS->id() && mergedLink.to() == link.to());

                    Signature *s = this->_getSignature(link.to());
                    if (s)
                    {
                        // modify neighbor "from"
                        s->removeLink(oldS->id());
                        s->addLink(mergedLink.inverse());

                        newS->addLink(mergedLink);
                    }
                    else
                    {
                        UERROR("Didn't find neighbor %d of %d in RAM...", link.to(), oldS->id());
                    }
                }
                newS->setLabel(oldS->getLabel());
                oldS->setLabel("");
                oldS->removeLinks(); // remove all links
                oldS->addLink(Link(oldS->id(), newS->id(), Link::kGlobalClosure, Transform(), 1, 1)); // to keep track of the merged location

                // Set old image to new signature
                this->copyData(oldS, newS);

                // update weight
                newS->setWeight(newS->getWeight() + 1 + oldS->getWeight());

                if (_lastGlobalLoopClosureId == oldS->id())
                {
                    _lastGlobalLoopClosureId = newS->id();
                }
            }
            else
            {
                newS->addLink(Link(newS->id(), oldS->id(), Link::kGlobalClosure, Transform() , 1, 1)); // to keep track of the merged location

                // update weight
                oldS->setWeight(newS->getWeight() + 1 + oldS->getWeight());

                if (_lastSignature == newS)
                {
                    _lastSignature = oldS;
                }
            }

            // remove location
            moveToTrash(_idUpdatedToNewOneRehearsal ? oldS : newS, _notLinkedNodesKeptInDb);

            return true;
        }
        else
        {
            // update only weights
            if (_idUpdatedToNewOneRehearsal)
            {
                // just update weight
                int w = oldS->getWeight() >= 0 ? oldS->getWeight() : 0;
                newS->setWeight(w + newS->getWeight() + 1);
                oldS->setWeight(intermediateMerge ? -1 : 0); // convert to intermediate node

                if (_lastGlobalLoopClosureId == oldS->id())
                {
                    _lastGlobalLoopClosureId = newS->id();
                }
            }
            else // !_idUpdatedToNewOneRehearsal
            {
                int w = newS->getWeight() >= 0 ? newS->getWeight() : 0;
                oldS->setWeight(w + oldS->getWeight() + 1);
                newS->setWeight(intermediateMerge ? -1 : 0); // convert to intermediate node
            }
        }
    }
    else
    {
        if (!newS)
        {
            UERROR("newId=%d, oldId=%d, Signature %d not found in working/st memories", newId, oldId, newId);
        }
        if (!oldS)
        {
            UERROR("newId=%d, oldId=%d, Signature %d not found in working/st memories", newId, oldId, oldId);
        }
    }
    return false;
}

Transform MemoryLoc::getOdomPose(int signatureId, bool lookInDatabase) const
{
    Transform pose, groundTruth;
    int mapId, weight;
    std::string label;
    double stamp;
    getNodeInfo(signatureId, pose, mapId, weight, label, stamp, groundTruth, lookInDatabase);
    return pose;
}

Transform MemoryLoc::getGroundTruthPose(int signatureId, bool lookInDatabase) const
{
    Transform pose, groundTruth;
    int mapId, weight;
    std::string label;
    double stamp;
    getNodeInfo(signatureId, pose, mapId, weight, label, stamp, groundTruth, lookInDatabase);
    return groundTruth;
}

bool MemoryLoc::getNodeInfo(int signatureId,
                            Transform &odomPose,
                            int &mapId,
                            int &weight,
                            std::string &label,
                            double &stamp,
                            Transform &groundTruth,
                            bool lookInDatabase) const
{
    const Signature *s = this->getSignature(signatureId);
    if (s)
    {
        odomPose = s->getPose();
        mapId = s->mapId();
        weight = s->getWeight();
        label = s->getLabel();
        stamp = s->getStamp();
        groundTruth = s->getGroundTruthPose();
        return true;
    }
    else if (lookInDatabase && _dbDriver)
    {
        return _dbDriver->getNodeInfo(signatureId, odomPose, mapId, weight, label, stamp, groundTruth);
    }
    return false;
}

SensorData MemoryLoc::getNodeData(int nodeId, bool uncompressedData, bool keepLoadedDataInMemory)
{
    UDEBUG("nodeId=%d", nodeId);
    SensorData r;
    Signature *s = this->_getSignature(nodeId);
    if (s && !s->sensorData().imageCompressed().empty())
    {
        if (keepLoadedDataInMemory && uncompressedData)
        {
            s->sensorData().uncompressData();
        }
        r = s->sensorData();
        if (!keepLoadedDataInMemory && uncompressedData)
        {
            r.uncompressData();
        }
    }
    else if (_dbDriver)
    {
        // load from database
        if (s && keepLoadedDataInMemory)
        {
            std::list<Signature *> signatures;
            signatures.push_back(s);
            _dbDriver->loadNodeData(signatures);
            if (uncompressedData)
            {
                s->sensorData().uncompressData();
            }
            r = s->sensorData();
        }
        else
        {
            _dbDriver->getNodeData(nodeId, r);
            if (uncompressedData)
            {
                r.uncompressData();
            }
        }
    }

    return r;
}

void MemoryLoc::getNodeWords(int nodeId,
                             std::multimap<int, cv::KeyPoint> &words,
                             std::multimap<int, cv::Point3f> &words3)
{
    UDEBUG("nodeId=%d", nodeId);
    Signature *s = this->_getSignature(nodeId);
    if (s)
    {
        words = s->getWords();
        words3 = s->getWords3();
    }
    else if (_dbDriver)
    {
        // load from database
        std::list<Signature *> signatures;
        std::list<int> ids;
        ids.push_back(nodeId);
        std::set<int> loadedFromTrash;
        _dbDriver->loadSignatures(ids, signatures, &loadedFromTrash);
        if (signatures.size())
        {
            words = signatures.front()->getWords();
            words3 = signatures.front()->getWords3();
            if (loadedFromTrash.size())
            {
                //put back
                _dbDriver->asyncSave(signatures.front());
            }
            else
            {
                delete signatures.front();
            }
        }
    }
}

int MemoryLoc::getNi(int signatureId) const
{
    int ni = 0;
    const Signature *s = this->getSignature(signatureId);
    if (s)
    {
        ni = (int)((Signature *)s)->getWords().size();
    }
    else
    {
        _dbDriver->getInvertedIndexNi(signatureId, ni);
    }
    return ni;
}


void MemoryLoc::copyData(const Signature *from, Signature *to)
{
    UTimer timer;
    timer.start();
    if (from && to)
    {
        // words 2d
        this->disableWordsRef(to->id());
        to->setWords(from->getWords());
        std::list<int> id;
        id.push_back(to->id());
        this->enableWordsRef(id);

        if (from->isSaved() && _dbDriver)
        {
            _dbDriver->getNodeData(from->id(), to->sensorData());
            UDEBUG("Loaded image data from database");
        }
        else
        {
            to->sensorData() = (SensorData)from->sensorData();
        }
        to->sensorData().setId(to->id());

        to->setPose(from->getPose());
        to->setWords3(from->getWords3());
        to->setWordsDescriptors(from->getWordsDescriptors());
    }
    else
    {
        ULOGGER_ERROR("Can't merge the signatures because there are not same type.");
    }
    UDEBUG("Merging time = %fs", timer.ticks());
}

class PreUpdateThread : public UThreadNode
{
public:
    PreUpdateThread(VWDictionary *vwp) : _vwp(vwp) {}
    virtual ~PreUpdateThread() {}
private:
    void mainLoop()
    {
        if (_vwp)
        {
            _vwp->update();
        }
        this->kill();
    }
    VWDictionary *_vwp;
};

Signature *MemoryLoc::createSignature(const SensorData &data, const Transform &pose, Statistics *stats)
{
    UDEBUG("");
    UASSERT(data.imageRaw().empty() ||
            data.imageRaw().type() == CV_8UC1 ||
            data.imageRaw().type() == CV_8UC3);
    UASSERT_MSG(data.depthOrRightRaw().empty() ||
                ((data.depthOrRightRaw().type() == CV_16UC1 ||
                  data.depthOrRightRaw().type() == CV_32FC1 ||
                  data.depthOrRightRaw().type() == CV_8UC1)
                 &&
                 ((data.imageRaw().empty() && data.depthOrRightRaw().type() != CV_8UC1) ||
                  (data.imageRaw().rows % data.depthOrRightRaw().rows == 0 && data.imageRaw().cols % data.depthOrRightRaw().cols == 0))),
                uFormat("image=(%d/%d) depth=(%d/%d, type=%d [accepted=%d,%d,%d])",
                        data.imageRaw().cols,
                        data.imageRaw().rows,
                        data.depthOrRightRaw().cols,
                        data.depthOrRightRaw().rows,
                        data.depthOrRightRaw().type(),
                        CV_16UC1, CV_32FC1, CV_8UC1).c_str());
    UASSERT(data.laserScanRaw().empty() || data.laserScanRaw().type() == CV_32FC2 || data.laserScanRaw().type() == CV_32FC3 || data.laserScanRaw().type() == CV_32FC(6));

    if (!data.depthOrRightRaw().empty() &&
            data.cameraModels().size() == 0 &&
            !data.stereoCameraModel().isValidForProjection())
    {
        UERROR("Rectified images required! Calibrate your camera.");
        return 0;
    }
    UASSERT(_feature2D != 0);

    PreUpdateThread preUpdateThread(_vwd);

    UTimer timer;
    timer.start();
    float t;
    std::vector<cv::KeyPoint> keypoints;
    cv::Mat descriptors;
    bool isIntermediateNode = data.id() < 0 || data.imageRaw().empty();
    int id = data.id();
    if (_generateIds)
    {
        id = this->getNextId();
    }
    else
    {
        if (id <= 0)
        {
            UERROR("Received image ID is null. "
                   "Please set parameter Mem/GenerateIds to \"true\" or "
                   "make sure the input source provides image ids (seq).");
            return 0;
        }
        else if (id > _idCount)
        {
            _idCount = id;
        }
        else
        {
            UERROR("Id of acquired image (%d) is smaller than the last in memory (%d). "
                   "Please set parameter Mem/GenerateIds to \"true\" or "
                   "make sure the input source provides image ids (seq) over the last in "
                   "memory, which is %d.",
                   id,
                   _idCount,
                   _idCount);
            return 0;
        }
    }

    int treeSize = int(_workingMem.size() + _stMem.size());
    int meanWordsPerLocation = 0;
    if (treeSize > 0)
    {
        meanWordsPerLocation = _vwd->getTotalActiveReferences() / treeSize;
    }

    if (_parallelized)
    {
        UDEBUG("Start dictionary update thread");
        preUpdateThread.start();
    }

    std::vector<cv::Point3f> keypoints3D;
    if (!_useOdometryFeatures || data.keypoints().empty() || (int)data.keypoints().size() != data.descriptors().rows)
    {
        if (_feature2D->getMaxFeatures() >= 0 && !data.imageRaw().empty() && !isIntermediateNode)
        {
            UINFO("Extract features");
            cv::Mat imageMono;
            if (data.imageRaw().channels() == 3)
            {
                cv::cvtColor(data.imageRaw(), imageMono, CV_BGR2GRAY);
            }
            else
            {
                imageMono = data.imageRaw();
            }

            cv::Mat depthMask;
            if (!data.depthRaw().empty() &&
                    _feature2D->getType() != Feature2D::kFeatureOrb) // ORB's mask pyramids don't seem to work well
            {
                if (imageMono.rows % data.depthRaw().rows == 0 &&
                        imageMono.cols % data.depthRaw().cols == 0 &&
                        imageMono.rows / data.depthRaw().rows == imageMono.cols / data.depthRaw().cols)
                {
                    depthMask = util2d::interpolate(data.depthRaw(), imageMono.rows / data.depthRaw().rows, 0.1f);
                }
            }

            keypoints = _feature2D->generateKeypoints(
                            imageMono,
                            depthMask);
            t = timer.ticks();
            if (stats) stats->addStatistic(Statistics::kTimingMemKeypoints_detection(), t * 1000.0f);
            UDEBUG("time keypoints (%d) = %fs", (int)keypoints.size(), t);

            descriptors = _feature2D->generateDescriptors(imageMono, keypoints);
            t = timer.ticks();
            if (stats) stats->addStatistic(Statistics::kTimingMemDescriptors_extraction(), t * 1000.0f);
            UDEBUG("time descriptors (%d) = %fs", descriptors.rows, t);

            UDEBUG("ratio=%f, meanWordsPerLocation=%d", _badSignRatio, meanWordsPerLocation);
            if (descriptors.rows && descriptors.rows < _badSignRatio * float(meanWordsPerLocation))
            {
                descriptors = cv::Mat();
            }
            else if ((!data.depthRaw().empty() && data.cameraModels().size() && data.cameraModels()[0].isValidForProjection()) ||
                     (!data.rightRaw().empty() && data.stereoCameraModel().isValidForProjection()))
            {
                keypoints3D = _feature2D->generateKeypoints3D(data, keypoints);
                if (_feature2D->getMinDepth() > 0.0f || _feature2D->getMaxDepth() > 0.0f)
                {
                    UDEBUG("");
                    //remove all keypoints/descriptors with no valid 3D points
                    UASSERT((int)keypoints.size() == descriptors.rows &&
                            keypoints3D.size() == keypoints.size());
                    std::vector<cv::KeyPoint> validKeypoints(keypoints.size());
                    std::vector<cv::Point3f> validKeypoints3D(keypoints.size());
                    cv::Mat validDescriptors(descriptors.size(), descriptors.type());

                    int oi = 0;
                    for (unsigned int i = 0; i < keypoints3D.size(); ++i)
                    {
                        if (util3d::isFinite(keypoints3D[i]))
                        {
                            validKeypoints[oi] = keypoints[i];
                            validKeypoints3D[oi] = keypoints3D[i];
                            descriptors.row(i).copyTo(validDescriptors.row(oi));
                            ++oi;
                        }
                    }
                    UDEBUG("Removed %d invalid 3D points", (int)keypoints3D.size() - oi);
                    validKeypoints.resize(oi);
                    validKeypoints3D.resize(oi);
                    keypoints = validKeypoints;
                    keypoints3D = validKeypoints3D;
                    descriptors = validDescriptors.rowRange(0, oi).clone();
                }
                t = timer.ticks();
                if (stats) stats->addStatistic(Statistics::kTimingMemKeypoints_3D(), t * 1000.0f);
                UDEBUG("time keypoints 3D (%d) = %fs", (int)keypoints3D.size(), t);
            }
        }
        else if (data.imageRaw().empty())
        {
            UDEBUG("Empty image, cannot extract features...");
        }
        else if (_feature2D->getMaxFeatures() < 0)
        {
            UDEBUG("_feature2D->getMaxFeatures()(%d<0) so don't extract any features...", _feature2D->getMaxFeatures());
        }
        else
        {
            UDEBUG("Intermediate node detected, don't extract features!");
        }
    }
    else if (_feature2D->getMaxFeatures() >= 0 && !isIntermediateNode)
    {
        UINFO("Use odometry features");
        keypoints = data.keypoints();
        descriptors = data.descriptors().clone();

        UASSERT(descriptors.empty() || descriptors.rows == (int)keypoints.size());

        if ((int)keypoints.size() > _feature2D->getMaxFeatures())
        {
            _feature2D->limitKeypoints(keypoints, descriptors, _feature2D->getMaxFeatures());
        }

        if (descriptors.empty())
        {
            cv::Mat imageMono;
            if (data.imageRaw().channels() == 3)
            {
                cv::cvtColor(data.imageRaw(), imageMono, CV_BGR2GRAY);
            }
            else
            {
                imageMono = data.imageRaw();
            }

            descriptors = _feature2D->generateDescriptors(imageMono, keypoints);
            t = timer.ticks();
            if (stats) stats->addStatistic(Statistics::kTimingMemDescriptors_extraction(), t * 1000.0f);
            UDEBUG("time descriptors (%d) = %fs", descriptors.rows, t);
        }

        if ((!data.depthRaw().empty() && data.cameraModels().size() && data.cameraModels()[0].isValidForProjection()) ||
                (!data.rightRaw().empty() && data.stereoCameraModel().isValidForProjection()))
        {
            keypoints3D = _feature2D->generateKeypoints3D(data, keypoints);
            if (_feature2D->getMinDepth() > 0.0f || _feature2D->getMaxDepth() > 0.0f)
            {
                UDEBUG("");
                //remove all keypoints/descriptors with no valid 3D points
                UASSERT((int)keypoints.size() == descriptors.rows &&
                        keypoints3D.size() == keypoints.size());
                std::vector<cv::KeyPoint> validKeypoints(keypoints.size());
                std::vector<cv::Point3f> validKeypoints3D(keypoints.size());
                cv::Mat validDescriptors(descriptors.size(), descriptors.type());

                int oi = 0;
                for (unsigned int i = 0; i < keypoints3D.size(); ++i)
                {
                    if (util3d::isFinite(keypoints3D[i]))
                    {
                        validKeypoints[oi] = keypoints[i];
                        validKeypoints3D[oi] = keypoints3D[i];
                        descriptors.row(i).copyTo(validDescriptors.row(oi));
                        ++oi;
                    }
                }
                UDEBUG("Removed %d invalid 3D points", (int)keypoints3D.size() - oi);
                validKeypoints.resize(oi);
                validKeypoints3D.resize(oi);
                keypoints = validKeypoints;
                keypoints3D = validKeypoints3D;
                descriptors = validDescriptors.rowRange(0, oi).clone();
            }
            t = timer.ticks();
            if (stats) stats->addStatistic(Statistics::kTimingMemKeypoints_3D(), t * 1000.0f);
            UDEBUG("time keypoints 3D (%d) = %fs", (int)keypoints3D.size(), t);
        }

        UDEBUG("ratio=%f, meanWordsPerLocation=%d", _badSignRatio, meanWordsPerLocation);
        if (descriptors.rows && descriptors.rows < _badSignRatio * float(meanWordsPerLocation))
        {
            descriptors = cv::Mat();
        }
    }

    if (_parallelized)
    {
        UDEBUG("Joining dictionary update thread...");
        preUpdateThread.join(); // Wait the dictionary to be updated
        UDEBUG("Joining dictionary update thread... thread finished!");
    }

    std::list<int> wordIds;
    if (descriptors.rows)
    {
        t = timer.ticks();
        if (stats) stats->addStatistic(Statistics::kTimingMemJoining_dictionary_update(), t * 1000.0f);
        if (_parallelized)
        {
            UDEBUG("time descriptor and memory update (%d of size=%d) = %fs", descriptors.rows, descriptors.cols, t);
        }
        else
        {
            UDEBUG("time descriptor (%d of size=%d) = %fs", descriptors.rows, descriptors.cols, t);
        }

        wordIds = _vwd->addNewWords(descriptors, id);
        t = timer.ticks();
        if (stats) stats->addStatistic(Statistics::kTimingMemAdd_new_words(), t * 1000.0f);
        UDEBUG("time addNewWords %fs", t);
    }
    else if (id > 0)
    {
        UDEBUG("id %d is a bad signature", id);
    }

    std::multimap<int, cv::KeyPoint> words;
    std::multimap<int, cv::Point3f> words3D;
    std::multimap<int, cv::Mat> wordsDescriptors;
    if (wordIds.size() > 0)
    {
        UASSERT(wordIds.size() == keypoints.size());
        UASSERT(keypoints3D.size() == 0 || keypoints3D.size() == wordIds.size());
        unsigned int i = 0;
        for (std::list<int>::iterator iter = wordIds.begin(); iter != wordIds.end() && i < keypoints.size(); ++iter, ++i)
        {
            if (_imageDecimation > 1)
            {
                cv::KeyPoint kpt = keypoints[i];
                kpt.pt.x /= float(_imageDecimation);
                kpt.pt.y /= float(_imageDecimation);
                kpt.size /= float(_imageDecimation);
                words.insert(std::pair<int, cv::KeyPoint>(*iter, kpt));
            }
            else
            {
                words.insert(std::pair<int, cv::KeyPoint>(*iter, keypoints[i]));
            }
            if (keypoints3D.size())
            {
                words3D.insert(std::pair<int, cv::Point3f>(*iter, keypoints3D.at(i)));
            }
            if (_rawDescriptorsKept)
            {
                wordsDescriptors.insert(std::pair<int, cv::Mat>(*iter, descriptors.row(i).clone()));
            }
        }
    }

    if (!pose.isNull() &&
            data.cameraModels().size() == 1 &&
            words.size() &&
            words3D.size() == 0)
    {
        bool fillWithNaN = true;
        if (_signatures.size())
        {
            UDEBUG("Generate 3D words using odometry");
            Signature *previousS = _signatures.rbegin()->second;
            if (previousS->getWords().size() > 8 && words.size() > 8 && !previousS->getPose().isNull())
            {
                Transform cameraTransform = pose.inverse() * previousS->getPose();
                // compute 3D words by epipolar geometry with the previous signature
                std::map<int, cv::Point3f> inliers = util3d::generateWords3DMono(
                        uMultimapToMapUnique(words),
                        uMultimapToMapUnique(previousS->getWords()),
                        data.cameraModels()[0],
                        cameraTransform);

                // words3D should have the same size than words
                float bad_point = std::numeric_limits<float>::quiet_NaN();
                for (std::multimap<int, cv::KeyPoint>::const_iterator iter = words.begin(); iter != words.end(); ++iter)
                {
                    std::map<int, cv::Point3f>::iterator jter = inliers.find(iter->first);
                    if (jter != inliers.end())
                    {
                        words3D.insert(std::make_pair(iter->first, jter->second));
                    }
                    else
                    {
                        words3D.insert(std::make_pair(iter->first, cv::Point3f(bad_point, bad_point, bad_point)));
                    }
                }

                t = timer.ticks();
                UASSERT(words3D.size() == words.size());
                if (stats) stats->addStatistic(Statistics::kTimingMemKeypoints_3D(), t * 1000.0f);
                UDEBUG("time keypoints 3D (%d) = %fs", (int)words3D.size(), t);
                fillWithNaN = false;
            }
        }
        if (fillWithNaN)
        {
            float bad_point = std::numeric_limits<float>::quiet_NaN();
            for (std::multimap<int, cv::KeyPoint>::const_iterator iter = words.begin(); iter != words.end(); ++iter)
            {
                words3D.insert(std::make_pair(iter->first, cv::Point3f(bad_point, bad_point, bad_point)));
            }
        }
    }

    cv::Mat image = data.imageRaw();
    cv::Mat depthOrRightImage = data.depthOrRightRaw();
    std::vector<CameraModel> cameraModels = data.cameraModels();
    StereoCameraModel stereoCameraModel = data.stereoCameraModel();

    // apply decimation?
    if (_imageDecimation > 1)
    {
        image = util2d::decimate(image, _imageDecimation);
        depthOrRightImage = util2d::decimate(depthOrRightImage, _imageDecimation);
        for (unsigned int i = 0; i < cameraModels.size(); ++i)
        {
            cameraModels[i] = cameraModels[i].scaled(1.0 / double(_imageDecimation));
        }
        if (stereoCameraModel.isValidForProjection())
        {
            stereoCameraModel.scale(1.0 / double(_imageDecimation));
        }
    }

    // downsampling the laser scan?
    cv::Mat laserScan = data.laserScanRaw();
    int maxLaserScanMaxPts = data.laserScanMaxPts();
    if (!laserScan.empty() && _laserScanDownsampleStepSize > 1)
    {
        laserScan = util3d::downsample(laserScan, _laserScanDownsampleStepSize);
        maxLaserScanMaxPts /= _laserScanDownsampleStepSize;
    }

    Signature *s;
    if (this->isBinDataKept())
    {
        UDEBUG("Bin data kept: rgb=%d, depth=%d, scan=%d, userData=%d",
               image.empty() ? 0 : 1,
               depthOrRightImage.empty() ? 0 : 1,
               laserScan.empty() ? 0 : 1,
               data.userDataRaw().empty() ? 0 : 1);

        std::vector<unsigned char> imageBytes;
        std::vector<unsigned char> depthBytes;

        if (_saveDepth16Format && !depthOrRightImage.empty() && depthOrRightImage.type() == CV_32FC1)
        {
            UWARN("Save depth data to 16 bits format: depth type detected is 32FC1, use 16UC1 depth format to avoid this conversion (or set parameter \"Mem/SaveDepth16Format\"=false to use 32bits format).");
            depthOrRightImage = util2d::cvtDepthFromFloat(depthOrRightImage);
        }

        rtabmap::CompressionThread ctImage(image, std::string(".jpg"));
        rtabmap::CompressionThread ctDepth(depthOrRightImage, std::string(".png"));
        rtabmap::CompressionThread ctLaserScan(laserScan);
        rtabmap::CompressionThread ctUserData(data.userDataRaw());
        ctImage.start();
        ctDepth.start();
        ctLaserScan.start();
        ctUserData.start();
        ctImage.join();
        ctDepth.join();
        ctLaserScan.join();
        ctUserData.join();

        s = new Signature(id,
                          _idMapCount,
                          isIntermediateNode ? -1 : 0, // tag intermediate nodes as weight=-1
                          data.stamp(),
                          "",
                          pose,
                          data.groundTruth(),
                          stereoCameraModel.isValidForProjection() ?
                          SensorData(
                              ctLaserScan.getCompressedData(),
                              maxLaserScanMaxPts,
                              data.laserScanMaxRange(),
                              ctImage.getCompressedData(),
                              ctDepth.getCompressedData(),
                              stereoCameraModel,
                              id,
                              0,
                              ctUserData.getCompressedData()) :
                          SensorData(
                              ctLaserScan.getCompressedData(),
                              maxLaserScanMaxPts,
                              data.laserScanMaxRange(),
                              ctImage.getCompressedData(),
                              ctDepth.getCompressedData(),
                              cameraModels,
                              id,
                              0,
                              ctUserData.getCompressedData()));
    }
    else
    {
        // just compress laser and user data
        rtabmap::CompressionThread ctLaserScan(laserScan);
        rtabmap::CompressionThread ctUserData(data.userDataRaw());
        ctLaserScan.start();
        ctUserData.start();
        ctLaserScan.join();
        ctUserData.join();

        s = new Signature(id,
                          _idMapCount,
                          isIntermediateNode ? -1 : 0, // tag intermediate nodes as weight=-1
                          data.stamp(),
                          "",
                          pose,
                          data.groundTruth(),
                          stereoCameraModel.isValidForProjection() ?
                          SensorData(
                              ctLaserScan.getCompressedData(),
                              maxLaserScanMaxPts,
                              data.laserScanMaxRange(),
                              cv::Mat(),
                              cv::Mat(),
                              stereoCameraModel,
                              id,
                              0,
                              ctUserData.getCompressedData()) :
                          SensorData(
                              ctLaserScan.getCompressedData(),
                              maxLaserScanMaxPts,
                              data.laserScanMaxRange(),
                              cv::Mat(),
                              cv::Mat(),
                              cameraModels,
                              id,
                              0,
                              ctUserData.getCompressedData()));
    }

    s->setWords(words);
    s->setWords3(words3D);
    s->setWordsDescriptors(wordsDescriptors);

    // set raw data
    s->sensorData().setImageRaw(image);
    s->sensorData().setDepthOrRightRaw(depthOrRightImage);
    s->sensorData().setLaserScanRaw(laserScan, maxLaserScanMaxPts, data.laserScanMaxRange());
    s->sensorData().setUserDataRaw(data.userDataRaw());

    s->sensorData().setGroundTruth(data.groundTruth());

    t = timer.ticks();
    if (stats) stats->addStatistic(Statistics::kTimingMemCompressing_data(), t * 1000.0f);
    UDEBUG("time compressing data (id=%d) %fs", id, t);
    if (words.size())
    {
        s->setEnabled(true); // All references are already activated in the dictionary at this point (see _vwd->addNewWords())
    }
    return s;
}

void MemoryLoc::disableWordsRef(int signatureId)
{
    UDEBUG("id=%d", signatureId);

    Signature *ss = this->_getSignature(signatureId);
    if (ss && ss->isEnabled())
    {
        const std::multimap<int, cv::KeyPoint> &words = ss->getWords();
        const std::list<int> &keys = uUniqueKeys(words);
        int count = _vwd->getTotalActiveReferences();
        // First remove all references
        for (std::list<int>::const_iterator i = keys.begin(); i != keys.end(); ++i)
        {
            _vwd->removeAllWordRef(*i, signatureId);
        }

        count -= _vwd->getTotalActiveReferences();
        ss->setEnabled(false);
        UDEBUG("%d words total ref removed from signature %d... (total active ref = %d)", count, ss->id(), _vwd->getTotalActiveReferences());
    }
}

void MemoryLoc::cleanUnusedWords()
{
    if (_vwd->isIncremental())
    {
        std::vector<VisualWord *> removedWords = _vwd->getUnusedWords();
        UDEBUG("Removing %d words (dictionary size=%d)...", removedWords.size(), _vwd->getVisualWords().size());
        if (removedWords.size())
        {
            // remove them from the dictionary
            _vwd->removeWords(removedWords);

            for (unsigned int i = 0; i < removedWords.size(); ++i)
            {
                if (_dbDriver)
                {
                    _dbDriver->asyncSave(removedWords[i]);
                }
                else
                {
                    delete removedWords[i];
                }
            }
        }
    }
}

void MemoryLoc::enableWordsRef(const std::list<int> &signatureIds)
{
    UDEBUG("size=%d", signatureIds.size());
    UTimer timer;
    timer.start();

    std::map<int, int> refsToChange; //<oldWordId, activeWordId>

    std::set<int> oldWordIds;
    std::list<Signature *> surfSigns;
    for (std::list<int>::const_iterator i = signatureIds.begin(); i != signatureIds.end(); ++i)
    {
        Signature *ss = dynamic_cast<Signature *>(this->_getSignature(*i));
        if (ss && !ss->isEnabled())
        {
            surfSigns.push_back(ss);
            std::list<int> uniqueKeys = uUniqueKeys(ss->getWords());

            //Find words in the signature which they are not in the current dictionary
            for (std::list<int>::const_iterator k = uniqueKeys.begin(); k != uniqueKeys.end(); ++k)
            {
                if (_vwd->getWord(*k) == 0 && _vwd->getUnusedWord(*k) == 0)
                {
                    oldWordIds.insert(oldWordIds.end(), *k);
                }
            }
        }
    }

    UDEBUG("oldWordIds.size()=%d, getOldIds time=%fs", oldWordIds.size(), timer.ticks());

    // the words were deleted, so try to math it with an active word
    std::list<VisualWord *> vws;
    if (oldWordIds.size() && _dbDriver)
    {
        // get the descriptors
        _dbDriver->loadWords(oldWordIds, vws);
    }
    UDEBUG("loading words(%d) time=%fs", oldWordIds.size(), timer.ticks());


    if (vws.size())
    {
        //Search in the dictionary
        std::vector<int> vwActiveIds = _vwd->findNN(vws);
        UDEBUG("find active ids (number=%d) time=%fs", vws.size(), timer.ticks());
        int i = 0;
        for (std::list<VisualWord *>::iterator iterVws = vws.begin(); iterVws != vws.end(); ++iterVws)
        {
            if (vwActiveIds[i] > 0)
            {
                //UDEBUG("Match found %d with %d", (*iterVws)->id(), vwActiveIds[i]);
                refsToChange.insert(refsToChange.end(), std::pair<int, int>((*iterVws)->id(), vwActiveIds[i]));
                if ((*iterVws)->isSaved())
                {
                    delete(*iterVws);
                }
                else if (_dbDriver)
                {
                    _dbDriver->asyncSave(*iterVws);
                }
            }
            else
            {
                //add to dictionary
                _vwd->addWord(*iterVws); // take ownership
            }
            ++i;
        }
        UDEBUG("Added %d to dictionary, time=%fs", vws.size() - refsToChange.size(), timer.ticks());

        //update the global references map and update the signatures reactivated
        for (std::map<int, int>::const_iterator iter = refsToChange.begin(); iter != refsToChange.end(); ++iter)
        {
            //uInsert(_wordRefsToChange, (const std::pair<int, int>)*iter); // This will be used to change references in the database
            for (std::list<Signature *>::iterator j = surfSigns.begin(); j != surfSigns.end(); ++j)
            {
                (*j)->changeWordsRef(iter->first, iter->second);
            }
        }
        UDEBUG("changing ref, total=%d, time=%fs", refsToChange.size(), timer.ticks());
    }

    int count = _vwd->getTotalActiveReferences();

    // Reactivate references and signatures
    for (std::list<Signature *>::iterator j = surfSigns.begin(); j != surfSigns.end(); ++j)
    {
        const std::vector<int> &keys = uKeys((*j)->getWords());
        if (keys.size())
        {
            const VisualWord *wordFirst = _vwd->getWord(keys.front());  //get descriptor size
            UASSERT(wordFirst != 0);
            //Descriptors used for MemoryLoc::computeTransform()
            cv::Mat descriptors(keys.size(), wordFirst->getDescriptor().cols, wordFirst->getDescriptor().type());
            // Add all references
            for (unsigned int i = 0; i < keys.size(); ++i)
            {
                _vwd->addWordRef(keys.at(i), (*j)->id());
                const VisualWord *word = _vwd->getWord(keys.at(i));
                UASSERT(word != 0);

                word->getDescriptor().copyTo(descriptors.row(i));

            }
            (*j)->sensorData().setFeatures(std::vector<cv::KeyPoint>(), descriptors);
            (*j)->setEnabled(true);
        }
    }

    count = _vwd->getTotalActiveReferences() - count;
    UDEBUG("%d words total ref added from %d signatures, time=%fs...", count, surfSigns.size(), timer.ticks());
}

// return all non-null poses
// return unique links between nodes (for neighbors: old->new, for loops: parent->child)
void MemoryLoc::getMetricConstraints(
    const std::set<int> &ids,
    std::map<int, Transform> &poses,
    std::multimap<int, Link> &links,
    bool lookInDatabase)
{
    UDEBUG("");
    for (std::set<int>::const_iterator iter = ids.begin(); iter != ids.end(); ++iter)
    {
        Transform pose = getOdomPose(*iter, lookInDatabase);
        if (!pose.isNull())
        {
            poses.insert(std::make_pair(*iter, pose));
        }
    }

    for (std::set<int>::const_iterator iter = ids.begin(); iter != ids.end(); ++iter)
    {
        if (uContains(poses, *iter))
        {
            std::map<int, Link> tmpLinks = getLinks(*iter, lookInDatabase);
            for (std::map<int, Link>::iterator jter = tmpLinks.begin(); jter != tmpLinks.end(); ++jter)
            {
                if (jter->second.isValid() &&
                        uContains(poses, jter->first) &&
                        graph::findLink(links, *iter, jter->first) == links.end())
                {
                    if (!lookInDatabase &&
                            (jter->second.type() == Link::kNeighbor ||
                             jter->second.type() == Link::kNeighborMerged))
                    {
                        Link link = jter->second;
                        const Signature *s = this->getSignature(jter->first);
                        UASSERT(s != 0);
                        while (s && s->getWeight() == -1)
                        {
                            // skip to next neighbor, well we assume that bad signatures
                            // are only linked by max 2 neighbor links.
                            std::map<int, Link> n = this->getNeighborLinks(s->id(), false);
                            UASSERT(n.size() <= 2);
                            std::map<int, Link>::iterator uter = n.upper_bound(s->id());
                            if (uter != n.end())
                            {
                                const Signature *s2 = this->getSignature(uter->first);
                                if (s2)
                                {
                                    link = link.merge(uter->second, uter->second.type());
                                    poses.erase(s->id());
                                    s = s2;
                                }

                            }
                            else
                            {
                                break;
                            }
                        }

                        links.insert(std::make_pair(*iter, link));
                    }
                    else
                    {
                        links.insert(std::make_pair(*iter, jter->second));
                    }
                }
            }
        }
    }
}
