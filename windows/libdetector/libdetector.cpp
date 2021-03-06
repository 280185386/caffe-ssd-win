// libdetector.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#include "caffe/caffe_layers_registry.hpp"
#include <caffe\caffe.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <algorithm>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <string>
#include <algorithm>

using namespace caffe;  // NOLINT(build/namespaces)
using namespace cv;
using std::string;
//////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////
class Detector {
public:
	Detector(
		const string& model_file,
		const string& weights_file,
		const string& label_file,
		const string& mean_file,
		const string& mean_value);

	std::vector<vector<float> > Detect(const cv::Mat& img);
	std::map<int, std::string> label_map;
private:
	void MakeLabels(const string& label_file);

	void SetMean(const string& mean_file, const string& mean_value);

	void WrapInputLayer(std::vector<cv::Mat>* input_channels);

	void Preprocess(const cv::Mat& img,
		std::vector<cv::Mat>* input_channels);

private:
	shared_ptr<Net<float>> net_;
	cv::Size input_geometry_;
	int num_channels_;
	cv::Mat mean_;
};

Detector::Detector(
	const string& model_file,
	const string& weights_file,
	const string& label_file,
	const string& mean_file,
	const string& mean_value) {

#ifdef CPU_ONLY
	Caffe::set_mode(Caffe::CPU);
#else
	Caffe::set_mode(Caffe::GPU);
#endif

	/* Load the network. */
	net_.reset(new Net<float>(model_file, TEST));
	net_->CopyTrainedLayersFrom(weights_file);

	CHECK_EQ(net_->num_inputs(), 1) << "Network should have exactly one input.";
	CHECK_EQ(net_->num_outputs(), 1) << "Network should have exactly one output.";

	Blob<float>* input_layer = net_->input_blobs()[0];
	num_channels_ = input_layer->channels();
	CHECK(num_channels_ == 3 || num_channels_ == 1)
		<< "Input layer should have 1 or 3 channels.";
	input_geometry_ = cv::Size(input_layer->width(), input_layer->height());

	/* Load the binaryproto mean file. */
	SetMean(mean_file, mean_value);
	MakeLabels(label_file);
}

std::vector<vector<float> > Detector::Detect(const cv::Mat& img) {
	Blob<float>* input_layer = net_->input_blobs()[0];
	input_layer->Reshape(1, num_channels_,
		input_geometry_.height, input_geometry_.width);
	/* Forward dimension change to all layers. */
	net_->Reshape();

	std::vector<cv::Mat> input_channels;
	WrapInputLayer(&input_channels);

	Preprocess(img, &input_channels);

	net_->Forward();

	/* Copy the output layer to a std::vector */
	Blob<float>* result_blob = net_->output_blobs()[0];
	const float* result = result_blob->cpu_data();
	const int num_det = result_blob->height();
	vector<vector<float> > detections;
	for (int k = 0; k < num_det; ++k) {
		if (result[0] == -1) {
			// Skip invalid detection.
			result += 7;
			continue;
		}
		vector<float> detection(result, result + 7);
		detections.push_back(detection);
		result += 7;
	}
	return detections;
}

void Detector::MakeLabels(const std::string& label_file) {
	if (label_file == "")
	{
		return;
	}
	std::ifstream infile(label_file.c_str());
	std::string line, label_id, label;
	while (infile >> line) {
		std::stringstream ss;
		ss.str(line);
		std::getline(ss, label_id, ',');
		std::getline(ss, label_id, ',');
		std::getline(ss, label, ',');
		label_map.insert(std::pair<int, std::string>(std::atoi(label_id.c_str()), label));
	}
}

/* Load the mean file in binaryproto format. */
void Detector::SetMean(const string& mean_file, const string& mean_value) {
	cv::Scalar channel_mean;
	if (!mean_file.empty()) {
		CHECK(mean_value.empty()) <<
			"Cannot specify mean_file and mean_value at the same time";
		BlobProto blob_proto;
		ReadProtoFromBinaryFileOrDie(mean_file.c_str(), &blob_proto);

		/* Convert from BlobProto to Blob<float> */
		Blob<float> mean_blob;
		mean_blob.FromProto(blob_proto);
		CHECK_EQ(mean_blob.channels(), num_channels_)
			<< "Number of channels of mean file doesn't match input layer.";

		/* The format of the mean file is planar 32-bit float BGR or grayscale. */
		std::vector<cv::Mat> channels;
		float* data = mean_blob.mutable_cpu_data();
		for (int i = 0; i < num_channels_; ++i) {
			/* Extract an individual channel. */
			cv::Mat channel(mean_blob.height(), mean_blob.width(), CV_32FC1, data);
			channels.push_back(channel);
			data += mean_blob.height() * mean_blob.width();
		}

		/* Merge the separate channels into a single image. */
		cv::Mat mean;
		cv::merge(channels, mean);

		/* Compute the global mean pixel value and create a mean image
		* filled with this value. */
		channel_mean = cv::mean(mean);
		mean_ = cv::Mat(input_geometry_, mean.type(), channel_mean);
	}
	if (!mean_value.empty()) {
		CHECK(mean_file.empty()) <<
			"Cannot specify mean_file and mean_value at the same time";
		stringstream ss(mean_value);
		vector<float> values;
		string item;
		while (getline(ss, item, ',')) {
			float value = std::atof(item.c_str());
			values.push_back(value);
		}
		CHECK(values.size() == 1 || values.size() == num_channels_) <<
			"Specify either 1 mean_value or as many as channels: " << num_channels_;

		std::vector<cv::Mat> channels;
		for (int i = 0; i < num_channels_; ++i) {
			/* Extract an individual channel. */
			cv::Mat channel(input_geometry_.height, input_geometry_.width, CV_32FC1,
				cv::Scalar(values[i]));
			channels.push_back(channel);
		}
		cv::merge(channels, mean_);
	}
}

/* Wrap the input layer of the network in separate cv::Mat objects
* (one per channel). This way we save one memcpy operation and we
* don't need to rely on cudaMemcpy2D. The last preprocessing
* operation will write the separate channels directly to the input
* layer. */
void Detector::WrapInputLayer(std::vector<cv::Mat>* input_channels) {
	Blob<float>* input_layer = net_->input_blobs()[0];

	int width = input_layer->width();
	int height = input_layer->height();
	float* input_data = input_layer->mutable_cpu_data();
	for (int i = 0; i < input_layer->channels(); ++i) {
		cv::Mat channel(height, width, CV_32FC1, input_data);
		input_channels->push_back(channel);
		input_data += width * height;
	}
}

void Detector::Preprocess(const cv::Mat& img,
	std::vector<cv::Mat>* input_channels) {
	/* Convert the input image to the input image format of the network. */
	cv::Mat sample;
	if (img.channels() == 3 && num_channels_ == 1)
		cv::cvtColor(img, sample, cv::COLOR_BGR2GRAY);
	else if (img.channels() == 4 && num_channels_ == 1)
		cv::cvtColor(img, sample, cv::COLOR_BGRA2GRAY);
	else if (img.channels() == 4 && num_channels_ == 3)
		cv::cvtColor(img, sample, cv::COLOR_BGRA2BGR);
	else if (img.channels() == 1 && num_channels_ == 3)
		cv::cvtColor(img, sample, cv::COLOR_GRAY2BGR);
	else
		sample = img;

	cv::Mat sample_resized;
	if (sample.size() != input_geometry_)
		cv::resize(sample, sample_resized, input_geometry_);
	else
		sample_resized = sample;

	cv::Mat sample_float;
	if (num_channels_ == 3)
		sample_resized.convertTo(sample_float, CV_32FC3);
	else
		sample_resized.convertTo(sample_float, CV_32FC1);

	cv::Mat sample_normalized;
	cv::subtract(sample_float, mean_, sample_normalized);

	/* This operation will write the separate BGR planes directly to the
	* input layer of the network because it is wrapped by the cv::Mat
	* objects in input_channels. */
	cv::split(sample_normalized, *input_channels);

	CHECK(reinterpret_cast<float*>(input_channels->at(0).data)
		== net_->input_blobs()[0]->cpu_data())
		<< "Input channels are not wrapping the input layer of the network.";
}

DEFINE_string(mean_file, "",
	"The mean file used to subtract from the input image.");
DEFINE_string(mean_value, "104,117,123",
	"If specified, can be one value or can be same as image channels"
	" - would subtract from the corresponding channel). Separated by ','."
	"Either mean_file or mean_value should be provided, not both.");
DEFINE_string(file_type, "image",
	"The file type in the list_file. Currently support image and video.");
DEFINE_string(out_file, "",
	"If provided, store the detection results in the out_file.");
DEFINE_double(confidence_threshold, 0.01,
	"Only store detections with score higher than the threshold.");
////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////
typedef std::pair<std::string, float> Prediction;

class Classifier {
public:
	Classifier(const std::string& model_file,
		const std::string& trained_file,
		const std::string& mean_file,
		const std::string& label_file);

	~Classifier();
	std::vector<Prediction> Classify(const cv::Mat& img, int N = 5);

private:
	void SetMean(const std::string& mean_file);

	std::vector<float> Predict(const cv::Mat& img);

	void WrapInputLayer(std::vector<cv::Mat>* input_channels);

	void Preprocess(const cv::Mat& img,
		std::vector<cv::Mat>* input_channels);

private:
	std::shared_ptr<caffe::Net<float>> net_;
	cv::Size input_geometry_;
	int num_channels_;
	cv::Mat mean_;
	float *mean_data;
	std::vector<std::string> labels_;
};


Classifier::Classifier(const string& model_file,
	const string& trained_file,
	const string& mean_file,
	const string& label_file) {

	Caffe::set_mode(Caffe::CPU);

	/* Load the network. */
	net_.reset(new Net<float>(model_file, TEST));
	net_->CopyTrainedLayersFrom(trained_file);

	CHECK_EQ(net_->num_inputs(), 1) << "Network should have exactly one input.";
	CHECK_EQ(net_->num_outputs(), 1) << "Network should have exactly one output.";

	Blob<float>* input_layer = net_->input_blobs()[0];
	num_channels_ = input_layer->channels();
	CHECK(num_channels_ == 3 || num_channels_ == 1)
		<< "Input layer should have 1 or 3 channels.";
	input_geometry_ = cv::Size(input_layer->width(), input_layer->height());

	/* Load the binaryproto mean file. */
	SetMean(mean_file);

	/* Load labels. */
	std::ifstream labels(label_file.c_str());
	CHECK(labels) << "Unable to open labels file " << label_file;
	string line;
	while (std::getline(labels, line))
		labels_.push_back(string(line));

	Blob<float>* output_layer = net_->output_blobs()[0];
	CHECK_EQ(labels_.size(), output_layer->channels())
		<< "Number of labels is different from the output layer dimension.";

	mean_data = NULL;
}

Classifier::~Classifier(){
	delete[]mean_data;
}
static bool PairCompare(const std::pair<float, int>& lhs,
	const std::pair<float, int>& rhs) {
	return lhs.first > rhs.first;
}

/* Return the indices of the top N values of vector v. */
static std::vector<int> Argmax(const std::vector<float>& v, int N) {
	std::vector<std::pair<float, int> > pairs;
	for (size_t i = 0; i < v.size(); ++i)
		pairs.push_back(std::make_pair(v[i], i));
	std::partial_sort(pairs.begin(), pairs.begin() + N, pairs.end(), PairCompare);

	std::vector<int> result;
	for (int i = 0; i < N; ++i)
		result.push_back(pairs[i].second);
	return result;
}

/* Return the top N predictions. */
std::vector<Prediction> Classifier::Classify(const cv::Mat& img, int N) {
	std::vector<float> output = Predict(img);

	N = std::min<int>(labels_.size(), N);
	std::vector<int> maxN = Argmax(output, N);
	std::vector<Prediction> predictions;
	for (int i = 0; i < N; ++i) {
		int idx = maxN[i];
		predictions.push_back(std::make_pair(labels_[idx], output[idx]));
	}

	return predictions;
}

/* Load the mean file in binaryproto format. */
void Classifier::SetMean(const string& mean_file) {
#ifdef USE_MEAN_FILE
	BlobProto blob_proto;
	ReadProtoFromBinaryFileOrDie(mean_file.c_str(), &blob_proto);

	/* Convert from BlobProto to Blob<float> */
	Blob<float> mean_blob;
	mean_blob.FromProto(blob_proto);
	CHECK_EQ(mean_blob.channels(), num_channels_)
		<< "Number of channels of mean file doesn't match input layer.";

	/* The format of the mean file is planar 32-bit float BGR or grayscale. */
	std::vector<cv::Mat> channels;
	float* data = mean_blob.mutable_cpu_data();
	for (int i = 0; i < num_channels_; ++i) {
		/* Extract an individual channel. */
		cv::Mat channel(mean_blob.height(), mean_blob.width(), CV_32FC1, data);
		channels.push_back(channel);
		data += mean_blob.height() * mean_blob.width();
	}

	/* Merge the separate channels into a single image. */
	cv::Mat mean;
	cv::merge(channels, mean);

	/* Compute the global mean pixel value and create a mean image
	* filled with this value. */
	cv::Scalar channel_mean = cv::mean(mean);
	mean_ = cv::Mat(input_geometry_, mean.type(), channel_mean);
#else
	float meanvalue[] = { 104.0f, 117.0f, 123.0f };
	float *mean_data = new float[num_channels_ * input_geometry_.height * input_geometry_.width];
	for (int i = 0; i < num_channels_ * input_geometry_.height * input_geometry_.width; i++)
	{
		if (i < input_geometry_.height * input_geometry_.width)
		{
			mean_data[i] = meanvalue[0];
			continue;
		}
		if (i < 2 * input_geometry_.height * input_geometry_.width)
		{
			mean_data[i] = meanvalue[1];
			continue;
		}
		mean_data[i] = meanvalue[2];
	}
	/////////////////////
	std::vector<cv::Mat> channels;
	for (int i = 0; i < num_channels_; ++i) {
		//std::fill(data, data + (i + 1) * input_geometry_.height * input_geometry_.width, meanvalue[i]);

		/* Extract an individual channel. */
		cv::Mat channel(input_geometry_.height, input_geometry_.width, CV_32FC1, mean_data);
		channels.push_back(channel);
		mean_data += input_geometry_.height * input_geometry_.width;
	}
	cv::Mat mean;
	cv::merge(channels, mean);
	cv::Scalar channel_mean = cv::mean(mean);
	mean_ = cv::Mat(input_geometry_, mean.type(), channel_mean);
#endif
}

std::vector<float> Classifier::Predict(const cv::Mat& img) {
	Blob<float>* input_layer = net_->input_blobs()[0];
	input_layer->Reshape(1, num_channels_,
		input_geometry_.height, input_geometry_.width);
	/* Forward dimension change to all layers. */
	net_->Reshape();

	std::vector<cv::Mat> input_channels;
	WrapInputLayer(&input_channels);

	Preprocess(img, &input_channels);

	net_->Forward();

	/* Copy the output layer to a std::vector */
	Blob<float>* output_layer = net_->output_blobs()[0];
	const float* begin = output_layer->cpu_data();
	const float* end = begin + output_layer->channels();
	return std::vector<float>(begin, end);
}

/* Wrap the input layer of the network in separate cv::Mat objects
* (one per channel). This way we save one memcpy operation and we
* don't need to rely on cudaMemcpy2D. The last preprocessing
* operation will write the separate channels directly to the input
* layer. */
void Classifier::WrapInputLayer(std::vector<cv::Mat>* input_channels) {
	Blob<float>* input_layer = net_->input_blobs()[0];

	int width = input_layer->width();
	int height = input_layer->height();
	float* input_data = input_layer->mutable_cpu_data();
	for (int i = 0; i < input_layer->channels(); ++i) {
		cv::Mat channel(height, width, CV_32FC1, input_data);
		input_channels->push_back(channel);
		input_data += width * height;
	}
}

void Classifier::Preprocess(const cv::Mat& img,
	std::vector<cv::Mat>* input_channels) {
	/* Convert the input image to the input image format of the network. */
	cv::Mat sample;
	if (img.channels() == 3 && num_channels_ == 1)
		cv::cvtColor(img, sample, cv::COLOR_BGR2GRAY);
	else if (img.channels() == 4 && num_channels_ == 1)
		cv::cvtColor(img, sample, cv::COLOR_BGRA2GRAY);
	else if (img.channels() == 4 && num_channels_ == 3)
		cv::cvtColor(img, sample, cv::COLOR_BGRA2BGR);
	else if (img.channels() == 1 && num_channels_ == 3)
		cv::cvtColor(img, sample, cv::COLOR_GRAY2BGR);
	else
		sample = img;

	cv::Mat sample_resized;
	if (sample.size() != input_geometry_)
		cv::resize(sample, sample_resized, input_geometry_);
	else
		sample_resized = sample;

	cv::Mat sample_float;
	if (num_channels_ == 3)
		sample_resized.convertTo(sample_float, CV_32FC3);
	else
		sample_resized.convertTo(sample_float, CV_32FC1);

	cv::Mat sample_normalized;
	cv::subtract(sample_float, mean_, sample_normalized);

	/* This operation will write the separate BGR planes directly to the
	* input layer of the network because it is wrapped by the cv::Mat
	* objects in input_channels. */
	cv::split(sample_normalized, *input_channels);

	CHECK(reinterpret_cast<float*>(input_channels->at(0).data)
		== net_->input_blobs()[0]->cpu_data())
		<< "Input channels are not wrapping the input layer of the network.";
}

///////////////////////////////////////////////////////////////////////////////////////////////
Classifier *classifier = NULL;
Detector *detector = NULL;

char model[] = "deploy.prototxt";
char trained[] = "VGG_VOC0712_SSD_300x300_iter_120000.caffemodel";
char mean_fn[] = "mean.binaryproto";
char label[] = "labels.txt";

__declspec(dllexport) void InitializeClassifier(std::string model_file, std::string trained_file, std::string mean_file, std::string label_file)
{
	classifier = new Classifier(model_file, trained_file, mean_file, label_file);
}

void InitializeClassifier()
{
	if (classifier == NULL)
		InitializeClassifier(model, trained, mean_fn, label);
}

void InitializeClassifier(const char* model_file, const char* trained_file, const char* mean_file, const char* label_file)
{
	if (classifier == NULL)
		classifier = new Classifier(string(model_file), string(trained_file), string(mean_file), string(label_file));
}

__declspec(dllexport) void InitializeDetector(std::string model_file, std::string trained_file, std::string label_file, std::string mean_file, std::string mean_value)
{
	if (mean_file == "")
		mean_file = FLAGS_mean_file;
	if (mean_value == "")
		mean_value = FLAGS_mean_value;

	detector = new Detector(model_file, trained_file, label_file, mean_file, mean_value);
}

string double2string(double value)
{
	std::ostringstream strs;
	strs << value;
	std::string str = strs.str();
	return str;
}

__declspec(dllexport) std::vector<std::string> recognizeImage(unsigned char * data = NULL,
	int width = 0,
	int height = 0)
{
	std::vector<std::string> result;
	if (classifier == NULL)
		return result;

	if (data == NULL || width <= 0 || height <= 0)
		return result;

	cv::Mat img(height, width, CV_8UC3, data);
	std::vector<Prediction> predictions = classifier->Classify(img);

	/* Print the top N predictions. */
	for (size_t i = 0; i < predictions.size(); ++i) {
		Prediction p = predictions[i];
		std::cout << std::fixed << std::setprecision(4) << p.second << " - \""
			<< p.first << "\"" << std::endl;
		result.push_back(p.first + " : " + double2string(p.second));
	}
	return result;
}

__declspec(dllexport) std::vector<vector<float>> detectObject(unsigned char * data = NULL,
	int width = 0,
	int height = 0)
{
	std::vector<vector<float>> detections;
	if (detector == NULL)
		return detections;

	if (data == NULL || width <= 0 || height <= 0)
		return detections;

	cv::Mat img(height, width, CV_8UC3, data);
	detections = detector->Detect(img);

	/* Print the top N predictions. */
	//for (int i = 0; i < detections.size(); ++i) {
	//	const vector<float>& d = detections[i];
	//	// Detection format: [image_id, label, score, xmin, ymin, xmax, ymax].
	//	CHECK_EQ(d.size(), 7);
	//	const float score = d[2];
	//	if (score >= 0.5) {
	//		int x, y, width, height;
	//		x = d[3] * img.cols;
	//		y = d[4] * img.rows;
	//		width = (d[5] - d[3]) * img.cols;
	//		height = (d[6] - d[4]) * img.rows;
	//		cv::rectangle(img, cv::Rect(x, y, width, height), cv::Scalar(0, 0, 255), 1, 1, 0);
	//	}
	//}
	return detections;
}

__declspec(dllexport) std::string getCaption(int classid)
{
	return detector->label_map[classid];
}

