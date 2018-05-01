#include "opencv/highgui.h"
#include "opencv2/opencv.hpp"
using namespace cv;
//inline int clamp(int val, int min_val, int max_val) {
//	if (val < min_val) return min_val;
//	if (val > max_val) return max_val;
//	return val;
//}
//void yuv2rgb(uint8_t yValue, uint8_t uValue, uint8_t vValue,uint8_t *r, uint8_t *g, uint8_t *b) {
//	int rTmp = yValue + (1.370705 * (vValue - 128));
//	int gTmp = yValue - (0.698001 * (vValue - 128)) - (0.337633 * (uValue - 128));
//	int bTmp = yValue + (1.732446 * (uValue - 128));
//	*r = clamp(rTmp, 0, 255);
//	*g = clamp(gTmp, 0, 255);
//	*b = clamp(bTmp, 0, 255);
//}
extern "C" __declspec(dllexport) int image_write(uint8_t *data , char *filePath,int width,int height) {
	Mat src;
	src = Mat::zeros(height, width, CV_8UC3);
	memcpy(src.data, data, 3 * width * height);
	// change memory space from YUY2 to UYVY --

	//imshow("test3", src);
	imwrite(filePath, src);
	return 1 ;
}
extern "C" __declspec(dllexport) int image_write_test(uint8_t *data, char *filePath, int width, int height) {
	Mat src;
	src = Mat::zeros(height, width, CV_8UC4);
	memcpy(src.data, data, 4 * width * height);
	cvtColor(src, src, COLOR_YUV2BGR);
	imwrite(filePath, src);
	return 1;
}