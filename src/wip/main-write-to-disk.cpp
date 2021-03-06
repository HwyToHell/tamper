// project specific
#include "../inc/backgroundsubtraction.h"
#include "../inc/motionbuffer.h"
#include "../inc/time-stamp.h"
#include "../inc/video-capture-simu.h"

// opencv
#include <opencv2/opencv.hpp>

// qt
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QString>

// std
#include <iostream>
#include <string>


// returns video file name
// start: enable saveToDisk at this frame number
// stop: disable saveToDisk at this frame number
static std::string writeToDiskTest(MotionBuffer& buf, VideoCaptureSimu& cap,
                     int start, int stop) {
    cv::Mat frame;
    for (int count = 0; count <= stop + 5; ++count) {
        std::cout << std::endl << "pass: " << count << std::endl;

        // buf = 1 (min value) -> first frame written = start+1
        if (count >= start && !buf.isSaveToDiskRunning())
            buf.setSaveToDisk(true);

        // stop must be start+1 or greater to generate log file
        if (count >= stop)
            buf.setSaveToDisk(false);

        cap.read(frame);
        std::cout << "frame read" << std::endl;
        buf.pushToBuffer(frame);
    }

    // TODO if saveToDiskRunning -> use blocking version of getMotionFileName
    std::string videoFileName = buf.getVideoFileName();
    std::cout << "new video file created: " << videoFileName << std::endl;
    return videoFileName;
}

// test MotionBuffer saveToDisk
int main_write_to_disk(int argc, char *argv[]) {
    (void)argc; (void)argv;
    using namespace std;

    const size_t sourceFps = 10;
    VideoCaptureSimu cap(sourceFps, "160x120");
    double readSourceFps = cap.get(cv::CAP_PROP_FPS);
    cout << "fps generated by source: " << readSourceFps << endl;


    const size_t bufSize = 3;
    MotionBuffer mb(bufSize, readSourceFps, "video", "log");

    const int startFrame = bufSize + 10;
    const int stopFrame = startFrame + 2;

    std::string videoFile = writeToDiskTest(mb, cap, startFrame, stopFrame);
    cap.release();

    cout << endl;
    cout << "writeToDisk started at frame: " << startFrame << endl;
    cout << "writeToDisk stopped at frame: " << stopFrame << endl;
    cout << "video file: " << videoFile << endl;
    cout << "param file: " << mb.getLogFileRelPath() << endl;

    return 0;
}
