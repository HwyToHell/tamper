#include "../inc/video-capture-simu.h"
#include "../inc/motionbuffer.h"
#include "../inc/time-stamp.h"

#include <cassert>
#include <functional>
#include <sstream>


VideoCaptureSimu::VideoCaptureSimu(InputMode inputMode, std::string videoSize, size_t framesPerSecond, bool logging) :
    m_availVideoSizes{
        {"160x120", cv::Size(160,120)},
        {"320x240", cv::Size(320,240)},
        {"640x480", cv::Size(640,480)},
        {"1024x768", cv::Size(1024,768)},
        {"1280x720", cv::Size(1280,720)},
        {"1920x1080", cv::Size(1920,1080)}
    },
    m_cntFrame{0},
    m_fps{framesPerSecond},     // default: 10
    m_frameSize{m_availVideoSizes.find("640x480")->second},
    m_genMode{GenMode::timeStamp},
    m_inputMode{inputMode},
    m_isLogging{logging},       //default: false
    m_isNewFrame{false},
    m_isRead{true},
    m_isReleased{false}
{
    selectVideoSize(videoSize);
    if (m_inputMode == InputMode::videoFile) {
        /* simulate reading from file with a very short delay of 1 ms */
        m_fps = 1000;
    }
    m_thread = std::thread(&VideoCaptureSimu::generateFrame, this);
}


VideoCaptureSimu::VideoCaptureSimu(std::string videoSize) :
    VideoCaptureSimu{InputMode::videoFile, videoSize, 0}
{}


VideoCaptureSimu::~VideoCaptureSimu()
{
    release();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    assert(!m_thread.joinable());
}

/*
 * return synthetic black frame
 */
cv::Mat createBlackFrame(cv::Size frameSize)
{
    const cv::Vec3b black  = cv::Vec3b(0,0,0);
    cv::Mat frame(frameSize, CV_8UC3, black);
    return frame;
}

/*
 * put time stamp with ms granularity on frame
 * format: yyyy-mm-dd_hh-mm-ss.ms
 */
void putTimeStamp(cv::Mat& frame)
{
    const cv::Vec3b red    = cv::Vec3b(0,0,255);
    const int thickness     = 1;

    std::string timeStamp = getTimeStamp(TimeResolution::ms);
    double fontScale = frame.size().width / 600.;
    int basLn;
    cv::Size textSize = cv::getTextSize(timeStamp, cv::FONT_HERSHEY_SIMPLEX,
                                        fontScale, thickness, &basLn);
    cv::Point textOrg(frame.size().width / 100,
                      frame.size().height / 50 + textSize.height);
    cv::putText(frame, timeStamp, textOrg, cv::FONT_HERSHEY_SIMPLEX, fontScale,
                red, thickness);
}

/*
 * put parametrizable motion area on black frame
 *********************************************************
 *                                   *  <- motionArea -> *
 *********************************************************
 */
void putMotionArea(cv::Mat& frame, int area, int greyLevel)
{
    // keep height, adjust width to fit area
    int widthMotionArea  = frame.size().width * area / 100;
    int xOrgAdj = frame.size().width - widthMotionArea -1;
    cv::Rect motionRect(xOrgAdj, 0, widthMotionArea, frame.size().height);

    // grey level -> color channel intensity (uchar)
    unsigned char chIntensity = static_cast<unsigned char>(255 * greyLevel / 100);
    cv::Vec3b greyShade = cv::Vec3b(chIntensity, chIntensity, chIntensity);

    cv::rectangle(frame, motionRect, greyShade, cv::FILLED, cv::LINE_4);
}



/*
 * generate synthetic frame according to frame rate in concurrent thread
 * new frame with printed time stamp watermark, frame count encoded in pixel(0,0)
 * and time stamp (ms) as int in pixel(0,height)
 * main thread notified after new frame available
 */
void VideoCaptureSimu::generateFrame()
{
    auto durationPerFrame = std::chrono::milliseconds(1000 / m_fps);
    if (m_isLogging) {
        std::cout << "frame duration: " << durationPerFrame.count() << std::endl;
    }

    std::chrono::system_clock::time_point startTimePoint = std::chrono::system_clock::now();

    while (!m_isReleased) {
        {
            std::lock_guard<std::mutex> newFrameLock(m_mtxNewFrame);
            switch (m_genMode) {
            case GenMode::black:
                m_sourceFrame = createBlackFrame(m_frameSize);
                break;
            case GenMode::timeStamp:
                m_sourceFrame = createBlackFrame(m_frameSize);
                putTimeStamp(m_sourceFrame);
                break;
            case GenMode::motionArea:
                m_sourceFrame = createBlackFrame(m_frameSize);
                putMotionArea(m_sourceFrame, m_motionArea, m_motionGreyLevel);
                break;
            case GenMode::motionAreaAndTime:
                m_sourceFrame = createBlackFrame(m_frameSize);
                putMotionArea(m_sourceFrame, m_motionArea, m_motionGreyLevel);
                putTimeStamp(m_sourceFrame);
            }

            durationPerFrame = std::chrono::milliseconds(1000 / m_fps);

            // encode frame count as int in first pixel
            m_sourceFrame.at<int>(0,0) = m_cntFrame;

            // encode time stamp in ms as int in first pixel of last row
            m_sourceFrame.at<int>(0,m_sourceFrame.rows-1) = static_cast<int>(elapsedMs(startTimePoint));

            m_isNewFrame = true;
            ++m_cntFrame;
            if (m_isLogging) {
                std::cout << getTimeStamp(TimeResolution::ms) << " new frame " << m_cntFrame << std::endl;
            }
        }
        m_cndNewFrame.notify_one();


        /* cam mode --> wait for time out (duration per frame) */
        if (m_inputMode == InputMode::camera) {
            std::unique_lock<std::mutex> stopLock(m_mtxStop);

            /* check stop condition for graceful thread suspension */
            if (m_cndStop.wait_for(stopLock, durationPerFrame, [this]{return m_isReleased;})) {
                if (m_isLogging) {
                    std::cout << getTimeStamp(TimeResolution::ms) << " stop thread in cam mode " << getFrameCount() << std::endl;
                }
                break;
            } else {
                if (m_isLogging) {
                    std::cout << getTimeStamp(TimeResolution::ms) << " timed out " << getFrameCount() << std::endl;
                }
            }

        /* video mode --> wait for frameRead */
        } else {

            /* wait for frame to be read */
            {
                std::unique_lock<std::mutex> readLock(m_mtxRead);
                m_cndRead.wait(readLock, [this]{return m_isRead;});
                m_isRead = false;
            }

            /* check stop condition for graceful thread suspension */
            {
                std::lock_guard<std::mutex> stopLock(m_mtxStop);
                if (m_isReleased) {
                    if (m_isLogging) {
                        std::cout << getTimeStamp(TimeResolution::ms) << " stop thread in video mode " << getFrameCount() << std::endl;
                    }
                    break;
                }
            }
        }
    } // while !m_isReleased
}


/*
 * read capture properties in same format as cv::VideoCapture
 */
double VideoCaptureSimu::get(int propid)
{
    switch (propid) {
    case cv::CAP_PROP_FPS:
        return m_fps;
    case cv::CAP_PROP_FRAME_HEIGHT:
        return m_frameSize.height;
    case cv::CAP_PROP_FRAME_WIDTH:
        return m_frameSize.width;
    case cv::CAP_PROP_MODE:
        return static_cast<double>(m_inputMode);
    default:
        return 0;
    }
}


int VideoCaptureSimu::getFrameCount()
{
    return m_cntFrame;
}


bool VideoCaptureSimu::read(cv::Mat& frame)
{
    {
        std::unique_lock<std::mutex> newFrameLock(m_mtxNewFrame);
        m_cndNewFrame.wait(newFrameLock, [this]{return m_isNewFrame;} );
        m_sourceFrame.copyTo(frame);
        m_isNewFrame = false;
    }

    if (m_isLogging) {
        std::cout << getTimeStamp(TimeResolution::ms) << " frame read " << getFrameCount() << std::endl;
    }

    /* video file input mode: fire frameRead event */
    if (m_inputMode==InputMode::videoFile) {
        {
            std::lock_guard<std::mutex> lock(m_mtxStop);
            m_isRead = true;
        }
        m_cndRead.notify_one();
    }
    return true;
}


void VideoCaptureSimu::release()
{
    /* camera input -> notify stop */
    {
        std::lock_guard<std::mutex> lock(m_mtxStop);
        m_isReleased = true;
    }
    m_cndStop.notify_one();  
    if (m_isLogging) {
        std::cout << getTimeStamp(TimeResolution::ms) << " release: stop notified " << getFrameCount() << std::endl;
    }

    /* video file input -> notify read */
    {
        std::lock_guard<std::mutex> lock(m_mtxRead);
        m_isRead = true;
    }
    m_cndRead.notify_one();
    if (m_isLogging) {
        std::cout << getTimeStamp(TimeResolution::ms) << " release: read notified " << getFrameCount() << std::endl;
    }
}


bool VideoCaptureSimu::selectVideoSize(std::string videoSize)
{
    for (std::pair<std::string, cv::Size> elem : m_availVideoSizes) {
        if (elem.first.find(videoSize) != std::string::npos) {
            m_frameSize = elem.second;
            std::cout << "video size: " << m_frameSize <<" selected" << std::endl;
            return true;
        }
    }
    std::cout << "video size: " << videoSize <<" not selectable" << std::endl;
    return false;
}


/*
 * write capture properties in same format as cv::VideoCapture
 */
bool VideoCaptureSimu::set(int propid, double value)
{
    switch (propid) {
        case cv::CAP_PROP_FPS: {

            /* set fps in camera mode only */
            if (m_inputMode == InputMode::videoFile) {
                std::cout << "fps cannot be set in video input mode" << std::endl;
                return false;
            } else {
                const size_t fpsMax = 60;
                const size_t fpsMin = 1;
                size_t fps = static_cast<size_t>(value);
                if (fps > fpsMax) {
                    m_fps = fpsMax;
                    std::cout << "fps set to max: " << fps << std::endl;
                } else {
                    if (fps < 1) {
                        m_fps = fpsMin;
                        std::cout << "fps set to min: " << fps << std::endl;
                    } else {
                        m_fps = fps;
                        std::cout << "fps set to: " << fps << std::endl;
                    }
                }
                return true;
            }
        }

        case cv::CAP_PROP_FRAME_WIDTH: {
            std::string width = std::to_string(static_cast<int>(value));
            return selectVideoSize(width);
        }

        case cv::CAP_PROP_FRAME_HEIGHT: {
            std::string height = std::to_string(static_cast<int>(value));
            return selectVideoSize(height);
        }

        default:
            return false;
    }
}


/*
 * generation mode: enum
 * area in per cent: 0 ... 100,         default = 0
 * grey level in per cent: 0 ... 100,   default = 0
 */
bool VideoCaptureSimu::setMode(GenMode mode, int area, int greyLevel)
{
    // boundary validation of input
    area = area > 100 ? 100 : area;
    area = area < 0 ? 0 : area;
    greyLevel = greyLevel > 100 ? 100 : greyLevel;
    greyLevel = greyLevel < 0 ? 0 : greyLevel;

    m_motionArea = area;
    m_motionGreyLevel = greyLevel;

    m_genMode = mode;

    return true;
}
