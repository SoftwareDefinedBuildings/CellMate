#include "run/run.h"
#include "lib/adapter/rtabmap/RTABMapAdapter.h"
#include "lib/data/Label.h"
#include "lib/data/Transform.h"
#include "lib/front_end/bosswave/BWFrontEnd.h"
#include "lib/front_end/http/HTTPFrontEnd.h"
#include "lib/util/Utility.h"
#include <QCoreApplication>
#include <cstdio>
#include <utility>

int Run::run(int argc, char *argv[]) {
  // Parse arguments
  bool http;
  int httpPort;
  bool bosswave;
  std::string bosswaveURI;
  int featureLimit;
  int corrLimit;
  double distRatio;
  std::vector<std::string> dbFiles;

  po::options_description visible("command options");
  visible.add_options() // use comment to force new line using formater
      ("help,h", "print help message") //
      ("http,H", po::value<bool>(&http)->default_value(true),
       "run HTTP front end") //
      ("http-port", po::value<int>(&httpPort)->default_value(8080),
       "the port that HTTP front end binds to") //
      ("bosswave,B", po::value<bool>(&bosswave)->default_value(false),
       "run BOSSWAVE front end") //
      ("bosswave-uri", po::value<std::string>(&bosswaveURI)
                           ->default_value("scratch.ns/cellmate"),
       "the URI that BOSSWAVE front end subscribes to") //
      ("feature-limit", po::value<int>(&featureLimit)->default_value(0),
       "limit the number of features used") //
      ("corr-limit", po::value<int>(&corrLimit)->default_value(0),
       "limit the number of corresponding 2D-3D points used") //
      ("dist-ratio", po::value<double>(&distRatio)->default_value(0.7),
       "distance ratio used to create words");

  po::options_description hidden;
  hidden.add_options() // use comment to force new line using formater
      ("dbfiles",
       po::value<std::vector<std::string>>(&dbFiles)->multitoken()->required(),
       "database files");

  po::options_description all;
  all.add(visible).add(hidden);

  po::positional_options_description pos;
  pos.add("dbfiles", -1);

  po::variables_map vm;
  po::parsed_options parsed = po::command_line_parser(argc, argv)
                                  .options(all)
                                  .positional(pos)
                                  .allow_unregistered()
                                  .run();
  po::store(parsed, vm);

  // print invalid options
  std::vector<std::string> unrecog =
      collect_unrecognized(parsed.options, po::exclude_positional);
  if (unrecog.size() > 0) {
    Run::printInvalid(unrecog);
    Run::printUsage(visible);
    return 1;
  }

  if (vm.count("help")) {
    Run::printUsage(visible);
    return 0;
  }

  // check whether required options exist after handling help
  po::notify(vm);

  // Run the program
  QCoreApplication app(argc, argv);

  std::cout << "reading data" << std::endl;
  RTABMapAdapter adapter;
  if (!adapter.init(std::set<std::string>(dbFiles.begin(), dbFiles.end()))) {
    std::cerr << "reading data failed";
    return 1;
  }

  const std::map<int, Word> &words = adapter.getWords();
  const std::map<int, Room> &rooms = adapter.getRooms();
  const std::map<int, std::vector<Label>> &labels = adapter.getLabels();

  std::cout << "initializing computing stages" << std::endl;
  _feature.reset(new Feature(featureLimit));
  _wordSearch.reset(new WordSearch(words));
  _roomSearch.reset(new RoomSearch(rooms, words));
  _perspective.reset(new Perspective(rooms, words, corrLimit, distRatio));
  _visibility.reset(new Visibility(labels));

  std::unique_ptr<FrontEnd> httpFrontEnd;
  if (http == true) {
    std::cout << "initializing HTTP front end" << std::endl;
    httpFrontEnd.reset(new HTTPFrontEnd(httpPort, MAX_CLIENTS));
    if (httpFrontEnd->start() == false) {
      std::cerr << "starting HTTP front end failed";
      return 1;
    }
    httpFrontEnd->registerOnQuery(std::bind(
        &Run::identify, this, std::placeholders::_1, std::placeholders::_2));
  }

  std::unique_ptr<FrontEnd> bwFrontEnd;
  if (bosswave == true) {
    std::cerr << "initializing BOSSWAVE front end" << std::endl;
    bwFrontEnd.reset(new BWFrontEnd(bosswaveURI));
    if (bwFrontEnd->start() == false) {
      std::cerr << "starting BOSSWAVE front end failed";
      return 1;
    }
    bwFrontEnd->registerOnQuery(std::bind(
        &Run::identify, this, std::placeholders::_1, std::placeholders::_2));
  }

  std::cout << "Initialization Done" << std::endl;

  return app.exec();
}

void Run::printInvalid(const std::vector<std::string> &opts) {
  std::cerr << "invalid options: ";
  for (const auto &opt : opts) {
    std::cerr << opt << " ";
  }
  std::cerr << std::endl;
}

void Run::printUsage(const po::options_description &desc) {
  std::cout << "cellmate run [command options] db_file..." << std::endl
            << std::endl
            << desc << std::endl;
}

void Run::printTime(long total, long feature, long wordSearch, long roomSearch,
                    long perspective, long visibility) {
  std::cout << "Time overall: " << total << " ms" << std::endl;
  std::cout << "Time feature: " << feature << " ms" << std::endl;
  std::cout << "Time wordSearch: " << wordSearch << " ms" << std::endl;
  std::cout << "Time roomSearch: " << roomSearch << " ms" << std::endl;
  std::cout << "Time perspective: " << perspective << " ms" << std::endl;
  std::cout << "Time visibility: " << visibility << " ms" << std::endl;
}

std::vector<std::string> Run::identify(const cv::Mat &image,
                                       const CameraModel &camera) {
  std::vector<std::string> results;

  long startTime;
  long totalStartTime = Utility::getTime();

  // feature extraction
  std::vector<cv::KeyPoint> keyPoints;
  cv::Mat descriptors;
  long featureTime;
  {
    std::lock_guard<std::mutex> lock(_featureMutex);
    startTime = Utility::getTime();
    _feature->extract(image, keyPoints, descriptors);
    featureTime = Utility::getTime() - startTime;
  }

  // word search
  std::vector<int> wordIds;
  long wordSearchTime;
  {
    std::lock_guard<std::mutex> lock(_wordSearchMutex);
    startTime = Utility::getTime();
    wordIds = _wordSearch->search(descriptors);
    wordSearchTime = Utility::getTime() - startTime;
  }

  // room search
  int roomId;
  long roomSearchTime;
  {
    std::lock_guard<std::mutex> lock(_roomSearchMutex);
    startTime = Utility::getTime();
    roomId = _roomSearch->search(wordIds);
    roomSearchTime = Utility::getTime() - startTime;
  }

  // PnP
  Transform pose;
  long perspectiveTime;
  {
    std::lock_guard<std::mutex> lock(_perspectiveMutex);
    startTime = Utility::getTime();
    _perspective->localize(wordIds, keyPoints, descriptors, camera, roomId,
                           pose);
    perspectiveTime = Utility::getTime() - startTime;
  }

  std::string OutputnName = "410Demo.txt";
  if (pose.isNull()) {
    long totalTime = Utility::getTime() - totalStartTime;
    Run::printTime(totalTime, featureTime, wordSearchTime, roomSearchTime,
                   perspectiveTime, -1);

    // for debug
    std::ofstream fout(OutputnName, std::ios_base::app);
    fout << "Null"
         << "\n";
    fout << "Null1\n";
    fout << "Null2\n";
    fout << "Null3\n";
    fout.close();

    return results;
  }

  // visibility
  long visibilityTime;
  {
    std::lock_guard<std::mutex> lock(_visibilityMutex);
    startTime = Utility::getTime();
    results = _visibility->process(roomId, camera, pose);
    visibilityTime = Utility::getTime() - startTime;
  }

  long totalTime = Utility::getTime() - totalStartTime;
  Run::printTime(totalTime, featureTime, wordSearchTime, roomSearchTime,
                 perspectiveTime, visibilityTime);

  // for debug
  std::ofstream fout(OutputnName, std::ios_base::app);
  fout << results.at(0) << "\n";
  fout << pose.r11() << "  " << pose.r12() << "   " << pose.r13() << "   "
       << pose.x() << "\n";
  fout << pose.r21() << "  " << pose.r22() << "   " << pose.r23() << "   "
       << pose.y() << "\n";
  fout << pose.r31() << "  " << pose.r32() << "   " << pose.r33() << "   "
       << pose.z() << "\n";
  fout.close();

  return results;
}