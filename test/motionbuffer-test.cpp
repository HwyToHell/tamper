#include "../inc/motionbuffer.h"
#include "../inc/video-capture-simu.h"
#include <catch.hpp>

#include <opencv2/opencv.hpp>
#include <opencv2/core/utils/filesystem.hpp>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;
const std::string logDir("log");
const bool        isLogging = true;
const std::string videoDir("videos");

struct LogFiles{
    std::string logFileRelPath;
    std::string videoFileRelPath;
};


/*
 * get value (time series) of a given key
 * fs: logfile (.json)
 * key: variable to extract samples from
 * returns vector of values
 */
std::vector<int> getBufferSamples(cv::FileStorage fs, std::string key) {
    std::vector<int> bufSamples;
    cv::FileNode root = fs.root();
    if (root.size() < 1) {
        std::cout << "root file node empty" << std::endl;
    }

    // key available?
    cv::FileNodeIterator itFirstNode = root.begin();
    std::vector<cv::String> keys = (*itFirstNode).keys();
    bool isKeyFound = false;
    for (auto k : keys) {
        if (k.compare(key) == 0) {
            isKeyFound = true;
        }
    }
    if (!isKeyFound) {
        std::cout << "file node does not contain key: " << key << std::endl;
        return bufSamples;
    }

    // populate vector with values associated with key
    for (cv::FileNodeIterator itSample = root.begin(); itSample != root.end(); ++itSample) {
        bufSamples.push_back(static_cast<int>((*itSample)[key]));
    }

    return bufSamples;
}


/*
 * returns video file name
 * start: enable saveToDisk frame number
 * stop: disable saveToDisk frame number
 */
LogFiles writeToDiskTest(VideoCaptureSimu& cap, size_t bufSize, double fpsOut,
                     int start, int stop, int overrun) {

    LogFiles logFiles;
    logFiles.logFileRelPath = logFiles.videoFileRelPath = "error";
    if (start >= stop || stop >= overrun) {
        std::cout << "start < stop < overrun" << std::endl;
        return logFiles;
    }

    MotionBuffer buf(bufSize, fpsOut, videoDir, logDir, isLogging);
    cv::Mat frame;
    for (int count = 0; count < overrun; ++count) {
        std::cout << std::endl << "pass: " << count << std::endl;

        // buf = 1 (min value) -> first frame written = start+1
        if (count >= start && !buf.isSaveToDiskRunning())
            buf.setSaveToDisk(true);

        // stop must be start+2 or greater to generate log file with values
        // start+1 generates empty log file only
        if (count >= stop)
            buf.setSaveToDisk(false);

        cap.read(frame);
        std::cout << "frame read" << std::endl;
        buf.pushToBuffer(frame);
    }

    // wait for post buffer to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // release necessary to finish post buffer and close video file
    buf.releaseBuffer();

    logFiles.logFileRelPath = buf.getLogFileRelPath();
    logFiles.videoFileRelPath = videoDir + '/' + buf.getVideoFileName();
    std::cout << "new video file created: " << logFiles.videoFileRelPath << std::endl;
    return logFiles;
}



// buffer size verification: determine number of written frames from logFile
// make sure to turn on logging in VideoCaptureSimu
// number of logged frames = buffer size + (stop - start)
TEST_CASE("TAM-18 buffer size verification", "[MotionBuffer][TAM-18]") {
    const size_t minBufSize = 1;
    const size_t maxBufSize = 60;

    std::string cwd = cv::utils::fs::getcwd();
    const size_t fps = 30;
    VideoCaptureSimu vcs(InputMode::camera, "160x120", fps, false);

    SECTION("preBufferSize below min") {
        const size_t bufSizeBelow = 0;
        const int startFrame = minBufSize + 10;

        LogFiles files = writeToDiskTest(vcs, bufSizeBelow, fps,
                                          startFrame, startFrame+1, startFrame+5);

        cv::FileStorage fs(files.logFileRelPath, cv::FileStorage::Mode::READ);
        std::vector<int> frmCounts = getBufferSamples(fs, "frame count");

        REQUIRE(frmCounts.size() == minBufSize + 1);
    }

    SECTION("preBufferSize in range") {
        const size_t bufSizeInRange = 30;
        const int startFrame = bufSizeInRange + 10;

        LogFiles files = writeToDiskTest(vcs, bufSizeInRange, fps,
                                          startFrame, startFrame+1, startFrame+5);

        cv::FileStorage fs(files.logFileRelPath, cv::FileStorage::Mode::READ);
        std::vector<int> frmCounts = getBufferSamples(fs, "frame count");

        // bufferSize + 1 frames logged (if frame rate is sufficiently low)
        REQUIRE(frmCounts.size() == bufSizeInRange + 1);
    }

    SECTION("preBufferSize above max, verify frame rate") {
        const size_t bufSizeAbove = 100;
        const int startFrame = maxBufSize + 10;

        LogFiles files = writeToDiskTest(vcs, bufSizeAbove, fps,
                                          startFrame, startFrame+1, startFrame+5);

        cv::FileStorage fs(files.logFileRelPath, cv::FileStorage::Mode::READ);
        std::vector<int> frmCounts = getBufferSamples(fs, "frame count");

        // bufferSize + 1 frames logged (if frame rate is sufficiently low)
        REQUIRE(frmCounts.size() == maxBufSize + 1);

        // TODO move monotonic and frame count verification to TAM-19
        // verify frame count increases monotonic
        for (size_t n = 1; n < frmCounts.size(); ++n) {
            REQUIRE((frmCounts[n] - frmCounts[n-1]) > 0);
        }

        // verify frame rate matches specification and stays the same
        std::vector<int> timeStamps = getBufferSamples(fs, "time stamp");
        double frameTimePrev = static_cast<double>(timeStamps[1] - timeStamps[0]);
        for (size_t n = 2; n < timeStamps.size(); ++n) {
            double frameTime = static_cast<double>(timeStamps[n] - timeStamps[n-1]);
            REQUIRE(frameTime == Approx(1000 / fps).epsilon(0.1));
            std::cout << "frame rate: " << frameTime << std::endl;
            REQUIRE(frameTimePrev == Approx(frameTime).epsilon(0.1));
            frameTimePrev = frameTime;
        }
    }
}


// verify buffer size and monotony in log file
TEST_CASE("TAM-19 ring buffer", "[MotionBuffer][TAM-19]") {
    const size_t bufSize    = 30;
    const size_t fpsInput   = 30;
    const double fpsOutput  = 30;
    const int    saveActive =  2; // saveToDisk must be active for at least two frames in order to generate log

    SECTION("videoFile input mode") {
        VideoCaptureSimu vcs(InputMode::videoFile, "160x120", fpsInput, false);
        const int startFrame = bufSize;
        const int largeOverrun = 200;

        LogFiles files = writeToDiskTest(vcs, bufSize, fpsOutput,
                                         startFrame,
                                         startFrame + saveActive,
                                         startFrame + largeOverrun);

        cv::FileStorage fs(files.logFileRelPath, cv::FileStorage::Mode::READ);
        std::vector<int> frmCounts = getBufferSamples(fs, "frame count");

        // post frames written = bufferSize + 1 (if buffer sufficiently filled)
        REQUIRE(frmCounts.size() == bufSize + 1);

        // verify frame count increases monotonic
        for (size_t n = 1; n < frmCounts.size(); ++n) {
            REQUIRE((frmCounts[n] - frmCounts[n-1]) > 0);
        }

        // calculate frame rate
        std::vector<int> timeStamps = getBufferSamples(fs, "time stamp");
        double fps = 1000 * timeStamps.size() / static_cast<double>(timeStamps.back() - timeStamps.front());
        std::cout << "fps videoFile input mode: " << fps << std::endl;

    } // SECTION videoFile input mode

    SECTION("camera input mode") {
        VideoCaptureSimu vcs(InputMode::camera, "160x120", fpsInput, false);
        const int startFrame = bufSize;
        const int overrun = 10;

        LogFiles files = writeToDiskTest(vcs, bufSize, fpsOutput,
                                         startFrame,
                                         startFrame + saveActive,
                                         startFrame + overrun);

        cv::FileStorage fs(files.logFileRelPath, cv::FileStorage::Mode::READ);
        std::vector<int> frmCounts = getBufferSamples(fs, "frame count");

        // post frames written = bufferSize + 1 (if buffer sufficiently filled)
        REQUIRE(frmCounts.size() == bufSize + 1);

        // verify frame count increases monotonic
        for (size_t n = 1; n < frmCounts.size(); ++n) {
            REQUIRE((frmCounts[n] - frmCounts[n-1]) > 0);
        }

        // verify frame rate matches specification and stays the same
        std::vector<int> timeStamps = getBufferSamples(fs, "time stamp");
        double frameTimePrev = static_cast<double>(timeStamps[1] - timeStamps[0]);
        for (size_t n = 2; n < timeStamps.size(); ++n) {
            double frameTime = static_cast<double>(timeStamps[n] - timeStamps[n-1]);
            REQUIRE(frameTime == Approx(1000 / fpsInput).epsilon(0.1));
            std::cout << "camera input mode frame time: " << frameTime << std::endl;
            REQUIRE(frameTimePrev == Approx(frameTime).epsilon(0.1));
            frameTimePrev = frameTime;
        }
    } // SECTION camera input mode

} // TEST_CASE TAM-19 ring buffer



// verify post buffer has been written and buffer released
// reasons for failure:
//   TAM-34 state::writePostBuffer does not finish, if releaseBuffer() not called
TEST_CASE("TAM-16 write post buffer", "[MotionBuffer][TAM-16]") {
    const size_t bufSize    = 30;
    const size_t fpsInput   = 30;
    const double fpsOutput  = 30;
    const int    saveActive =  2; // saveToDisk must be active for at least two frames in order to generate log
    std::string  cwd        = cv::utils::fs::getcwd();

    SECTION("videoFile input mode") {
        VideoCaptureSimu vcs(InputMode::videoFile, "160x120", fpsInput, false);

        SECTION("buffer full when starting saveMotionToDisk, "
                "sufficient new frames to empty buffer before destruction") {
            const int startFrame = bufSize;

            // many frames overrun, plenty of time to finish post buffer
            const int largeOverrun = 200;

            LogFiles files = writeToDiskTest(vcs, bufSize, fpsOutput,
                                             startFrame,
                                             startFrame + saveActive,
                                             startFrame + largeOverrun);

            cv::FileStorage fs(files.logFileRelPath, cv::FileStorage::Mode::READ);
            std::vector<int> frmCounts = getBufferSamples(fs, "frame count");

            // post frames written = bufferSize + 1 (if buffer sufficiently filled)
            REQUIRE(frmCounts.size() == bufSize + 1);

            // verify frame count increases monotonic
            for (size_t n = 1; n < frmCounts.size(); ++n) {
                REQUIRE((frmCounts[n] - frmCounts[n-1]) > 0);
            }
        }

        SECTION("buffer full when starting saveMotionToDisk, "
                "overrun ends immediately") {
            const int startFrame = bufSize;

            // overrun ends immediately
            LogFiles files = writeToDiskTest(vcs, bufSize, fpsOutput,
                                             startFrame,
                                             startFrame + saveActive,
                                             startFrame + saveActive + 1);

            cv::FileStorage fs(files.logFileRelPath, cv::FileStorage::Mode::READ);
            std::vector<int> frmCounts = getBufferSamples(fs, "frame count");

            REQUIRE(frmCounts.size() == bufSize + 1);

            // verify frame count increases monotonic
            for (size_t n = 1; n < frmCounts.size(); ++n) {
                REQUIRE((frmCounts[n] - frmCounts[n-1]) > 0);
            }
        }

        SECTION("buffer empty when starting saveMotionToDisk, "
                "overrun ends immediately") {
            // smallest possible buffer size
            const size_t zeroBuf = 1;
            const int startFrame = zeroBuf;

            LogFiles files = writeToDiskTest(vcs, bufSize, fpsOutput,
                                             startFrame,
                                             startFrame + saveActive,
                                             startFrame + saveActive + 1);

            cv::FileStorage fs(files.logFileRelPath, cv::FileStorage::Mode::READ);
            std::vector<int> frmCounts = getBufferSamples(fs, "frame count");

            REQUIRE(frmCounts.size() == zeroBuf + saveActive);
        }
    } // SECTION videoFile input mode

    SECTION("camera input mode") {
        VideoCaptureSimu vcs(InputMode::camera, "160x120", fpsInput, false);

        SECTION("buffer full when starting saveMotionToDisk, "
                "sufficient new frames to empty buffer before destruction") {
            const int startFrame = bufSize;

            // for cam running at 30 fps an overrun of 10 frames is sufficient
            const int overrun = 10;

            LogFiles files = writeToDiskTest(vcs, bufSize, fpsOutput,
                                             startFrame,
                                             startFrame + saveActive,
                                             startFrame + overrun);

            cv::FileStorage fs(files.logFileRelPath, cv::FileStorage::Mode::READ);
            std::vector<int> frmCounts = getBufferSamples(fs, "frame count");

            // post frames written = bufferSize + 1 (if buffer sufficiently filled)
            REQUIRE(frmCounts.size() == bufSize + 1);

            // verify frame count increases monotonic
            for (size_t n = 1; n < frmCounts.size(); ++n) {
                REQUIRE((frmCounts[n] - frmCounts[n-1]) > 0);
            }
        }

        SECTION("buffer full when starting saveMotionToDisk, "
                "overrun ends immediately") {
            const int startFrame = bufSize;

            // overrun ends immediately
            LogFiles files = writeToDiskTest(vcs, bufSize, fpsOutput,
                                             startFrame,
                                             startFrame + saveActive,
                                             startFrame + saveActive + 1);

            cv::FileStorage fs(files.logFileRelPath, cv::FileStorage::Mode::READ);
            std::vector<int> frmCounts = getBufferSamples(fs, "frame count");

            REQUIRE(frmCounts.size() == bufSize + 1);

            // verify frame count increases monotonic
            for (size_t n = 1; n < frmCounts.size(); ++n) {
                REQUIRE((frmCounts[n] - frmCounts[n-1]) > 0);
            }
        }

        SECTION("buffer empty when starting saveMotionToDisk, "
                "overrun ends immediately") {
            // smallest possible buffer size
            const size_t zeroBuf = 1;
            const int startFrame = zeroBuf;

            LogFiles files = writeToDiskTest(vcs, bufSize, fpsOutput,
                                             startFrame,
                                             startFrame + saveActive,
                                             startFrame + saveActive + 1);

            cv::FileStorage fs(files.logFileRelPath, cv::FileStorage::Mode::READ);
            std::vector<int> frmCounts = getBufferSamples(fs, "frame count");

            REQUIRE(frmCounts.size() == zeroBuf + saveActive);
        }

    } // SECTION camera input mode

} // TEST_CASE TAM-16 write post buffer


// compare set fpsOutput of motionBuffer with read fps of output file
// VideoCapture in camera mode
void verifyFps (size_t bufSize, size_t fpsSource, double fpsOutSet, double fpsOutTest,
                std::string frameSize) {
    VideoCaptureSimu vcs(InputMode::camera, frameSize, fpsSource);
    cv::VideoCapture cap;

    const int startFrame = static_cast<int>(bufSize);
    const int length = static_cast<int>(bufSize);

    // overrun must be big enough for post buffer to finish
    const int overrun = static_cast<int>(bufSize) + 10;
    LogFiles files = writeToDiskTest(vcs, bufSize, fpsOutSet,
                                         startFrame,
                                         startFrame + length,
                                         startFrame + length + overrun);

    double width = stod(frameSize.substr(0, frameSize.find('x')));
    double height = stod(frameSize.substr(frameSize.find('x')+1));

    cap.open(files.videoFileRelPath);
    REQUIRE(cap.isOpened() == true);
    REQUIRE(cap.get(cv::CAP_PROP_FPS) == Approx(fpsOutTest).epsilon(0.01));
    std::cout << "video file with " << cap.get(cv::CAP_PROP_FPS) << ": "
              << files.videoFileRelPath << std::endl;
    REQUIRE(cap.get(cv::CAP_PROP_FRAME_WIDTH) == Approx(width));
    REQUIRE(cap.get(cv::CAP_PROP_FRAME_HEIGHT) == Approx(height));
    cap.release();
}


// verify properties of written video file such as
// fps, frame order, frame size, output directory, file name
TEST_CASE("TAM-20 constructor: fps", "[MotionBuffer][TAM-20]") {
    const size_t sourceFps = 60;

    SECTION("smallest frame size") {
        std::string smallestFrame("160x120");

        SECTION("fps below min") {
            const double fpsBelowMin = 0;
            const double fpsMinTest = 1;
            const size_t bufferSize = 30;
            verifyFps(bufferSize, sourceFps, fpsBelowMin, fpsMinTest, smallestFrame);
        }

        SECTION("fps in range") {
            const double fpsInRange = 30;
            const size_t bufferSize = 30;
            verifyFps(bufferSize, sourceFps, fpsInRange, fpsInRange, smallestFrame);
        }

        SECTION("fps above max") {
            const double fpsAboveMax = 100;
            const double fpsMaxTest = 60;
            const size_t bufferSize = 30;
            verifyFps(bufferSize, sourceFps, fpsAboveMax, fpsMaxTest, smallestFrame);
        }
    }

    SECTION("largest frame size, highest fps") {
        std::string largestFrame("1920x1080");
        const size_t bufferSize = 60;
        const double fpsMax = 60;

        verifyFps(bufferSize, sourceFps, fpsMax, fpsMax, largestFrame);
    }
}


// read multiple video input files, use same buffer instance
TEST_CASE("TAM-35 process multiple video files", "[MotionBuffer][TAM-35]") {
    const size_t    fps         =  30;
    const size_t    bufSize     =  30;
    const int       startS2D    =  30;
    const int       stopS2D     = 100;
    const int       fileLen     = 135;

    VideoCaptureSimu vcs(InputMode::videoFile, "160x120", fps, false);
    MotionBuffer buf(bufSize, fps, videoDir, logDir, isLogging);
    cv::Mat frame;

    // 1st pass
    for (int count = 0; count < fileLen; ++count) {
        std::cout << std::endl << "pass: " << count << std::endl;

        if (count >= startS2D && !buf.isSaveToDiskRunning())
            buf.setSaveToDisk(true);

        if (count >= stopS2D)
            buf.setSaveToDisk(false);

        vcs.read(frame);
        buf.pushToBuffer(frame);
    }
    // wait for post buffer to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // release necessary to finish post buffer and close video file
    buf.releaseBuffer();

    // re-read file
    cv::VideoCapture cap;
    std::string videoFileRelPath = videoDir + '/' + buf.getVideoFileName();
    cap.open(videoFileRelPath);
    REQUIRE(cap.isOpened() == true);

    // save2disk frame count = pre-buffer + activeMotion + post-buffer
    // int s2dFrameCount = bufSize + (stopS2D - startS2D) + (fileLen - stopS2D);
    int s2dFrameCount = bufSize + (stopS2D - startS2D) + (bufSize);
    REQUIRE(cap.get(cv::CAP_PROP_FRAME_COUNT) == Approx(s2dFrameCount - 2).epsilon(0.01));
}

// clean up
TEST_CASE("TAM-31 delete temporary files", "[MotionBuffer][TearDown]") {
    std::string cwd = cv::utils::fs::getcwd();
    cv::utils::fs::remove_all(cwd + "/" + logDir);
    cv::utils::fs::remove_all(cwd + "/" + videoDir);
}

