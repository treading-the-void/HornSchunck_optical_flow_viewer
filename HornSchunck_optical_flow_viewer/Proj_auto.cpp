/**
 * @file Proj_auto.cpp
 * @brief 基于 Horn-Schunck 算法的光流可视化程序，支持视频/摄像头输入，实时显示光流箭头，
 *        并提供了丰富的参数调节和自动调参功能。
 *
 * 主要功能：
 * - 打开视频文件或摄像头
 * - 计算相邻帧之间的光流场 (Horn-Schunck 算法)
 * - 在画面上绘制彩色箭头表示光流方向与大小
 * - 提供六个滑动条手动调节关键参数
 * - 自动调参模式 (Auto Tune) 可根据当前平均光流大小动态调整参数
 * - 支持暂停/继续播放
 * - 将处理后的视频保存为 output_video.avi
 *
 * 编译依赖：OpenCV, Windows SDK (用于文件对话框)
 */

#include <opencv2/opencv.hpp>
#include <windows.h>
#include <shobjidl.h>   
#include <iostream>
#include <fstream>
#include <cmath>
#include <vector>
#include <locale>  
#include <codecvt> 

using namespace cv;
using namespace std;

// ========================= 全局变量 =========================

// 播放控制
bool paused = false;        // 是否暂停
bool autoTune = false;      // 是否开启自动调参
bool hasVideo = false;      // 是否已成功打开视频
bool inSubmenu = false;     // 是否处于二级菜单（打开文件/摄像头）

// 光流计算参数（由滑动条控制）
int sample_step = 15;           // 采样步长，控制绘制箭头的密度（步长越小箭头越多）
int alpha_slider = 10;          // 平滑系数 α，实际值为 alpha_slider/10.0，控制光流平滑度
int iter_slider = 50;           // 迭代次数，越大光流越收敛
int mag_skip_slider = 12;       // 光流幅度阈值（百分比），小于此值的光流不绘制箭头，避免噪声
int arrow_scale_slider = 2;     // 箭头缩放倍数，控制显示的光流长度
int maxMag = 1;                 // 颜色映射的最大幅度参考值，用于映射颜色

// 手动模式下的参数备份（用于 Auto Tune 切换时恢复）
int manual_alpha = 10;
int manual_iter = 50;
int manual_step = 15;
int manual_scale = 2;
int manual_maxMag = 1;
bool wasAutoTune = false;       // 记录上一次 autoTune 状态，用于检测切换

// 光流场数据
Mat prevGray;                   // 上一帧灰度图 (CV_32F)
Mat u, v;                       // 光流场 (x, y 分量)

// 视频相关
VideoCapture cap;               // 视频捕获对象
int frameIdx = 0;               // 当前处理帧序号
float fps = 0;                  // 实际处理帧率
float AVGMag = 0;               // 当前帧的平均光流幅度
Mat frame;                      // 当前帧 (BGR)
VideoWriter outputVideo;        // 输出视频写入器
bool videoFlag = true;          // 标记是否首次写入，用于初始化 VideoWriter

// ========================= 按钮结构 =========================

struct Button {
    Rect rect;                  // 按钮矩形区域
    string label;               // 按钮文字
    function<void()> onClick;   // 点击回调函数
    bool hovered = false;       // 鼠标是否悬停
};

vector<Button> buttons;         // 主菜单按钮
vector<Button> submenu;         // 二级菜单按钮

// ========================= 辅助函数 =========================

/**
 * @brief 将宽字符串转换为 UTF-8 字符串
 * @param wstr 宽字符串
 * @return std::string UTF-8 字符串
 */
std::string ConvertWstringToString(const std::wstring& wstr) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(wstr);
}

/**
 * @brief 绘制一个按钮
 * @param img 目标图像
 * @param btn 按钮对象
 */
void drawButton(Mat& img, const Button& btn) {
    // 背景色：悬停时亮蓝色，否则深灰色
    Scalar bg = btn.hovered ? Scalar(100, 200, 255) : Scalar(50, 50, 50);
    rectangle(img, btn.rect, bg, FILLED);
    rectangle(img, btn.rect, Scalar(200, 200, 200), 1);   // 边框
    putText(img, btn.label,
        Point(btn.rect.x + 5, btn.rect.y + btn.rect.height - 8),
        FONT_HERSHEY_SIMPLEX, 0.5, Scalar(255, 255, 255), 1);
}

/**
 * @brief 更新一组按钮的悬停状态
 * @param btns 按钮列表
 * @param x 鼠标 x 坐标
 * @param y 鼠标 y 坐标
 */
void updateHover(vector<Button>& btns, int x, int y) {
    for (auto& b : btns)
        b.hovered = b.rect.contains(Point(x, y));
}

/**
 * @brief 处理一组按钮的点击事件
 * @param btns 按钮列表
 * @param x 鼠标 x 坐标
 * @param y 鼠标 y 坐标
 */
void handleClick(vector<Button>& btns, int x, int y) {
    for (auto& b : btns)
        if (b.rect.contains(Point(x, y)) && b.onClick)
            b.onClick();
}

/**
 * @brief OpenCV 鼠标回调函数，处理悬停和点击
 */
void onMouse(int event, int x, int y, int, void*) {
    if (event == EVENT_MOUSEMOVE)
        updateHover(inSubmenu ? submenu : buttons, x, y);
    else if (event == EVENT_LBUTTONDOWN)
        handleClick(inSubmenu ? submenu : buttons, x, y);
}

/**
 * @brief 显示 Windows 文件选择对话框
 * @return 选中的文件路径 (宽字符串)
 */
wstring openFileDialog() {
    HRESULT hr;
    wstring path = L"";
    IFileOpenDialog* pFileOpen = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pFileOpen));
    if (SUCCEEDED(hr)) {
        hr = pFileOpen->Show(nullptr);
        if (SUCCEEDED(hr)) {
            IShellItem* pItem;
            if (SUCCEEDED(pFileOpen->GetResult(&pItem))) {
                PWSTR pszFilePath;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath))) {
                    path = pszFilePath;
                    CoTaskMemFree(pszFilePath);
                }
                pItem->Release();
            }
        }
        pFileOpen->Release();
    }
    return path;
}

/**
 * @brief 根据光流幅度计算颜色 (从红到蓝渐变)
 * @param mag 光流幅度
 * @return Scalar BGR 颜色值
 */
Scalar getColorFromMagnitude(float mag) {
    // 将幅度归一化到 [0,1]，使用 maxMag 控制颜色映射的敏感度
    float normMag = min(1.0f, mag / (float(maxMag) / 4));
    // 色相从 0 (红) 到 60 (黄) 渐变，幅度大的偏红，小的偏蓝
    int hue = static_cast<int>((1.0 - normMag) * 60);
    hue = max(0, min(59, hue));
    Mat hsv(1, 1, CV_8UC3, Scalar(hue, 255, 255));
    Mat bgr;
    cvtColor(hsv, bgr, COLOR_HSV2BGR);
    Vec3b color = bgr.at<Vec3b>(0, 0);
    return Scalar(color[0], color[1], color[2]);
}

/**
 * @brief 在图像上绘制单个光流箭头
 * @param img 目标图像
 * @param p 起始点
 * @param flow 光流向量 (dx, dy)
 * @param color 箭头颜色
 */
void drawArrow(Mat& img, Point2f p, Point2f flow, Scalar color) {
    Point2f q = p + flow * arrow_scale_slider;   // 缩放后的终点
    double angle = atan2(p.y - q.y, p.x - q.x);   // 箭头方向角
    line(img, p, q, color, 2, LINE_AA);
    // 绘制两个箭尾翼
    Point2f left = q + Point2f(6 * cos(angle + CV_PI / 6), 6 * sin(angle + CV_PI / 6));
    Point2f right = q + Point2f(6 * cos(angle - CV_PI / 6), 6 * sin(angle - CV_PI / 6));
    line(img, q, left, color, 2, LINE_AA);
    line(img, q, right, color, 2, LINE_AA);
}

/**
 * @brief Horn-Schunck 光流算法实现
 * @param prev 前一帧灰度图 (CV_32F)
 * @param next 当前帧灰度图 (CV_32F)
 * @param u 输出光流 x 分量
 * @param v 输出光流 y 分量
 * @param alpha 平滑系数 (越大越平滑)
 * @param iterations 迭代次数
 *
 * 核心公式：
 *   u = u_avg - Ix * (Ix*u_avg + Iy*v_avg + It) / (alpha^2 + Ix^2 + Iy^2)
 *   v = v_avg - Iy * (Ix*u_avg + Iy*v_avg + It) / (alpha^2 + Ix^2 + Iy^2)
 */
void computeHornSchunck(const Mat& prev, const Mat& next, Mat& u, Mat& v, float alpha, int iterations) {
    Mat Ix, Iy, It;
    // 计算空间梯度
    Sobel(prev, Ix, CV_32F, 1, 0, 3);
    Sobel(prev, Iy, CV_32F, 0, 1, 3);
    // 时间梯度
    It = next - prev;

    u = Mat::zeros(prev.size(), CV_32F);
    v = Mat::zeros(prev.size(), CV_32F);
    Mat uAvg, vAvg;
    float alpha2 = alpha * alpha;

    for (int k = 0; k < iterations; ++k) {
        // 对光流场进行邻域平均 (平滑)
        boxFilter(u, uAvg, -1, Size(3, 3));
        boxFilter(v, vAvg, -1, Size(3, 3));
        // 迭代更新每个像素
        for (int y = 0; y < prev.rows; ++y) {
            const float* ix = Ix.ptr<float>(y);
            const float* iy = Iy.ptr<float>(y);
            const float* it = It.ptr<float>(y);
            const float* ua = uAvg.ptr<float>(y);
            const float* va = vAvg.ptr<float>(y);
            float* up = u.ptr<float>(y);
            float* vp = v.ptr<float>(y);
            for (int x = 0; x < prev.cols; ++x) {
                float denom = alpha2 + ix[x] * ix[x] + iy[x] * iy[x];
                float num = ix[x] * ua[x] + iy[x] * va[x] + it[x];
                up[x] = ua[x] - ix[x] * num / denom;
                vp[x] = va[x] - iy[x] * num / denom;
            }
        }
    }
}

// ========================= 主函数 =========================

int main() {
    // 初始化 COM (用于文件对话框)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 主显示窗口
    namedWindow("Horn-Schunck", WINDOW_NORMAL);
    resizeWindow("Horn-Schunck", 960, 540);
    setMouseCallback("Horn-Schunck", onMouse);

    // 控制面板窗口 (滑动条)
    namedWindow("Controls", WINDOW_NORMAL);
    resizeWindow("Controls", 800, 300);
    moveWindow("Controls", 10, 10);

    // ========== 创建滑动条 (参数调节) ==========
    // 1. sample_step: 采样步长，值越小箭头越密集，计算量越大
    createTrackbar("Sample Step", "Controls", &sample_step, 50);
    // 2. alpha_slider: 平滑系数，实际值 = alpha_slider/10.0，越大光流越平滑
    createTrackbar("Alpha x10", "Controls", &alpha_slider, 100);
    // 3. iter_slider: 迭代次数，越大光流越收敛，但耗时增加
    createTrackbar("Iterations", "Controls", &iter_slider, 300);
    // 4. mag_skip_slider: 幅度阈值 (百分比)，小于此值的光流点将被跳过不绘制
    createTrackbar("Mag Skip", "Controls", &mag_skip_slider, 100);
    // 5. arrow_scale_slider: 箭头缩放倍数，控制显示光流长度
    createTrackbar("Arrow Scale", "Controls", &arrow_scale_slider, 40);
    // 6. maxMag: 颜色映射的最大参考幅度，用于调整颜色映射范围
    createTrackbar("color scl", "Controls", &maxMag, 10);

    // ========== 主菜单按钮 ==========
    buttons = {
        {{10, 10, 80, 28}, "Open", []() { inSubmenu = true; }},       // 打开二级菜单
        {{100, 10, 80, 28}, "Play", []() { paused = !paused; }},       // 暂停/继续
        {{190, 10, 90, 28}, "Auto Off", []() { autoTune = !autoTune; }}, // 自动调参开关
        {{290, 10, 80, 28}, "Exit", []() { destroyAllWindows(); exit(0); }}
    };

    // ========== 二级菜单 (打开方式) ==========
    submenu = {
        {{10, 10, 140, 30}, "Open Camera", []() {
            cap.open(0);                // 打开默认摄像头
            hasVideo = cap.isOpened();
            inSubmenu = false;
        }},
        {{160, 10, 140, 30}, "Open File", []() {
            wstring wpath = openFileDialog();
            if (!wpath.empty()) {
                cap.open(ConvertWstringToString(wpath));
                hasVideo = cap.isOpened();
            }
            inSubmenu = false;
        }},
        {{310, 10, 120, 30}, "Back", []() { inSubmenu = false; }}
    };

    // ========== 主循环 ==========
    while (true) {
        if (hasVideo && !paused) {
            // ----- 读取帧 -----
            cap >> frame;
            frameIdx++;
            if (frame.empty()) break;

            // ----- 初始化视频写入器 (只在第一帧) -----
            if (videoFlag) {
                // 使用 MJPG 编码，帧率为输入视频帧率，尺寸为当前帧尺寸
                outputVideo.open("output_video.avi",
                    VideoWriter::fourcc('M', 'J', 'P', 'G'),
                    cap.get(CAP_PROP_FPS),
                    frame.size());
                videoFlag = false;
            }

            double t1 = (double)getTickCount();   // 计时开始

            // ----- 转换为灰度图并归一化到 [0,1] -----
            Mat gray;
            cvtColor(frame, gray, COLOR_BGR2GRAY);
            gray.convertTo(gray, CV_32F, 1.0 / 255.0);

            // ----- 计算光流 (如果存在前一帧) -----
            if (!prevGray.empty()) {
                // 执行 Horn-Schunck 算法
                computeHornSchunck(prevGray, gray, u, v, alpha_slider / 10.0f, iter_slider);

                float avgMag = 0;
                int count = 0;

                // 遍历采样点，绘制箭头并统计平均幅度
                for (int y = sample_step; y < frame.rows - sample_step; y += sample_step) {
                    for (int x = sample_step; x < frame.cols - sample_step; x += sample_step) {
                        float dx = u.at<float>(y, x);
                        float dy = v.at<float>(y, x);
                        float mag = sqrt(dx * dx + dy * dy);
                        // 跳过幅度过小的点 (避免噪声)
                        if (mag < mag_skip_slider / 100.0f) continue;

                        // 绘制箭头，颜色根据幅度映射
                        drawArrow(frame, Point2f(x, y), Point2f(dx, dy), getColorFromMagnitude(mag));
                        avgMag += mag;
                        count++;
                    }
                }

                // 计算平均幅度
                if (count > 0) avgMag /= count;
                AVGMag = max(0.0f, avgMag);

                // ----- 自动调参逻辑 (Auto Tune) -----
                // 检测 autoTune 状态变化，以保存/恢复手动值
                if (autoTune != wasAutoTune) {
                    if (!autoTune) {
                        // 从 Auto On 切到 Auto Off：恢复之前保存的手动值
                        alpha_slider = manual_alpha;
                        iter_slider = manual_iter;
                        sample_step = manual_step;
                        arrow_scale_slider = manual_scale;
                        maxMag = manual_maxMag;
                    }
                    else {
                        // 从 Auto Off 切到 Auto On：保存当前手动值
                        manual_alpha = alpha_slider;
                        manual_iter = iter_slider;
                        manual_step = sample_step;
                        manual_scale = arrow_scale_slider;
                        manual_maxMag = maxMag;
                    }
                    wasAutoTune = autoTune;
                }

                // 若处于自动调参模式，根据当前平均幅度动态调整参数
                if (autoTune && count > 0) {
                    // 以下公式是经验调整，旨在使光流显示效果适应运动快慢
                    alpha_slider = max(1, ((avgMag + 0.1) * 40));            // 幅度大时增大平滑系数
                    iter_slider = min(200, max(10, int(avgMag * 80)));      // 幅度大时增加迭代次数
                    sample_step = max(5, int(avgMag * 70));                 // 幅度大时增大采样步长（减少箭头数量）
                    arrow_scale_slider = max(5, int(1.0 / (avgMag + 0.3) * 40)); // 幅度大时缩小箭头缩放
                    maxMag = max(1, int(1.0 / (avgMag + 1) * 3));           // 调整颜色映射范围
                }

                // 计算实际处理帧率 (FPS)
                double t2 = (double)getTickCount();
                fps = getTickFrequency() / (t2 - t1);
            }

            // 保存当前灰度图供下一帧使用
            prevGray = gray;

            // ----- 写入输出视频 (当前处理后的帧) -----
            if (outputVideo.isOpened() && !frame.empty()) {
                outputVideo.write(frame);
            }

        }
        else {
            // 无视频播放时显示占位信息
            frame = Mat::zeros(480, 640, CV_8UC3);
            putText(frame, "No video playing", Point(180, 240), FONT_HERSHEY_SIMPLEX, 1.0, Scalar(200, 200, 200), 2);
        }

        // ========== 绘制 UI 信息 ==========
        // 显示帧序号、平均光流幅度、实时帧率
        putText(frame, format("Frame: %d   Avg Flow: %.2f  FPS: %.2f", frameIdx, AVGMag, fps),
            Point(30, 70), FONT_HERSHEY_SIMPLEX, 0.7, Scalar(0, 0, 255), 2);

        // 更新按钮文字 (动态显示当前状态)
        buttons[1].label = paused ? "Play" : "Pause";
        buttons[2].label = autoTune ? "Auto On" : "Auto Off";

        // 绘制当前激活的菜单按钮
        for (auto& b : (inSubmenu ? submenu : buttons)) {
            drawButton(frame, b);
        }

        // ----- 显示 -----
        imshow("Horn-Schunck", frame);
        char key = waitKey(20);
        if (key == 27) break;   // ESC 退出
    }

    // ========== 清理资源 ==========
    cap.release();
    outputVideo.release();
    CoUninitialize();
    return 0;
}