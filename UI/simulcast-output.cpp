#include "simulcast-output.h"

#include <util/dstr.hpp>
#include <util/platform.h>
#include <util/profiler.hpp>
#include <util/util.hpp>
#include <obs-frontend-api.h>
#include <obs-app.hpp>
#include <obs.hpp>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include <QScopeGuard>
#include <QString>
#include <QThreadPool>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include "system-info.h"
#include "goliveapi-postdata.hpp"
#include "goliveapi-network.hpp"
#include "ivs-events.h"
#include "simulcast-error.h"

static const QString &device_id()
{
	static const QString device_id_ = []() -> QString {
		auto config = App()->GlobalConfig();
		if (!config_has_user_value(config, "General", "DeviceID")) {

			auto new_device_id = QUuid::createUuid().toString(
				QUuid::WithoutBraces);
			config_set_string(config, "General", "DeviceID",
					  new_device_id.toUtf8().constData());
		}
		return config_get_string(config, "General", "DeviceID");
	}();
	return device_id_;
}

static const QString &obs_session_id()
{
	static const QString session_id_ =
		QUuid::createUuid().toString(QUuid::WithoutBraces);
	return session_id_;
}

static void submit_event(BerryessaSubmitter *berryessa, const char *event_name,
			 obs_data_t *data)
{
	if (!berryessa)
		return;

	berryessa->submit(event_name, data);
}

static void add_always_string(BerryessaSubmitter *berryessa, const char *name,
			      const char *data)
{
	if (!berryessa)
		return;

	berryessa->setAlwaysString(name, data);
}

static OBSServiceAutoRelease create_service(const QString &device_id,
					    const QString &obs_session_id,
					    obs_data_t *go_live_config,
					    const QString &rtmp_url,
					    const QString &stream_key)
{
	const char *url = nullptr;
	QByteArray rtmp_url_utf8_data;
	if (!rtmp_url.isEmpty()) {
		rtmp_url_utf8_data = rtmp_url.toUtf8();
		url = rtmp_url_utf8_data.constData();

		blog(LOG_INFO, "Using custom rtmp URL: '%s'", url);
	} else {
		OBSDataArrayAutoRelease ingest_endpoints =
			obs_data_get_array(go_live_config, "ingest_endpoints");
		for (size_t i = 0; i < obs_data_array_count(ingest_endpoints);
		     i++) {
			OBSDataAutoRelease item =
				obs_data_array_item(ingest_endpoints, i);
			if (qstrnicmp("RTMP",
				      obs_data_get_string(item, "protocol"), 4))
				continue;

			url = obs_data_get_string(item, "url_template");
			blog(LOG_INFO, "Using URL template: '%s'", url);
			break;
		}

		if (!url) {
			blog(LOG_ERROR, "No RTMP URL in go live config");
			throw SimulcastError::warning(
				QTStr("FailedToStartStream.NoRTMPURLInConfig"));
		}
	}

	DStr str;
	dstr_cat(str, url);

	{
		auto found = dstr_find(str, "/{stream_key}");
		if (found)
			dstr_remove(str, found - str->array,
				    str->len - (found - str->array));
	}

	QUrl parsed_url{url};
	QUrlQuery parsed_query{parsed_url};

	parsed_query.addQueryItem("deviceIdentifier", device_id);
	parsed_query.addQueryItem("obsSessionId", obs_session_id);

	OBSDataAutoRelease go_live_meta =
		obs_data_get_obj(go_live_config, "meta");
	if (go_live_meta) {
		const char *config_id =
			obs_data_get_string(go_live_meta, "config_id");
		if (config_id && *config_id) {
			parsed_query.addQueryItem("obsConfigId", config_id);
		}
	}

	auto key_with_param = stream_key + "?" + parsed_query.toString();

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "server", str->array);
	obs_data_set_string(settings, "key",
			    key_with_param.toUtf8().constData());

	auto service = obs_service_create(
		"simulcast_service", "simulcast service", settings, nullptr);

	if (!service) {
		blog(LOG_WARNING, "Failed to create simulcast service");
		throw SimulcastError::warning(QTStr(
			"FailedToStartStream.FailedToCreateSimulcastService"));
	}

	return service;
}

static void ensure_directory_exists(std::string &path)
{
	replace(path.begin(), path.end(), '\\', '/');

	size_t last = path.rfind('/');
	if (last == std::string::npos)
		return;

	std::string directory = path.substr(0, last);
	os_mkdirs(directory.c_str());
}

std::string GetOutputFilename(const std::string &path, const char *format)
{
	std::string strPath;
	strPath += path;

	char lastChar = strPath.back();
	if (lastChar != '/' && lastChar != '\\')
		strPath += "/";

	strPath += BPtr<char>{
		os_generate_formatted_filename("flv", false, format)};
	ensure_directory_exists(strPath);

	return strPath;
}

static OBSOutputAutoRelease create_output(bool use_ertmp_multitrack)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_bool(settings, "ertmp_multitrack", use_ertmp_multitrack);

	OBSOutputAutoRelease output = obs_output_create(
		"rtmp_output", "rtmp simulcast", settings, nullptr);

	if (!output) {
		blog(LOG_ERROR, "failed to create simulcast rtmp output");
		throw SimulcastError::warning(QTStr(
			"FailedToStartStream.FailedToCreateSimulcastOutput"));
	}

	return output;
}

static OBSOutputAutoRelease create_recording_output(bool use_ertmp_multitrack)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "path",
			    GetOutputFilename(system_video_save_path(),
					      "%CCYY-%MM-%DD_%hh-%mm-%ss")
				    .c_str());
	obs_data_set_bool(settings, "ertmp_multitrack", use_ertmp_multitrack);

	OBSOutputAutoRelease output = obs_output_create(
		"flv_output", "flv simulcast", settings, nullptr);

	if (!output) {
		blog(LOG_ERROR, "failed to create simulcast flv output");
		throw SimulcastError::warning(
			"Failed to create simulcast flv output");
	}

	return output;
}

struct pixel_resolution {
	uint32_t width;
	uint32_t height;
};

static pixel_resolution scale_resolution(const obs_video_info &ovi,
					 uint64_t requested_width,
					 uint64_t requested_height)
{
	auto aspect_segments = std::gcd(ovi.base_width, ovi.base_height);
	auto aspect_width = ovi.base_width / aspect_segments;
	auto aspect_height = ovi.base_height / aspect_segments;

	auto base_pixels =
		static_cast<uint64_t>(ovi.base_width) * ovi.base_height;

	auto requested_pixels =
		std::min(requested_width * requested_height, base_pixels);

	auto pixel_ratio = std::min(
		requested_pixels / static_cast<double>(base_pixels), 1.0);

	auto target_aspect_segments = static_cast<uint32_t>(std::floor(
		std::sqrt(pixel_ratio * aspect_segments * aspect_segments)));

	if (target_aspect_segments + 1 * aspect_width > ovi.base_width)
		target_aspect_segments = ovi.base_width / aspect_width - 1;
	if (target_aspect_segments + 1 * aspect_height > ovi.base_height)
		target_aspect_segments = ovi.base_height / aspect_height - 1;

	for (auto i : {0, 1, -1}) {
		auto target_segments = std::max(
			static_cast<uint32_t>(1),
			std::min(aspect_segments, target_aspect_segments + i));
		auto output_width = aspect_width * target_segments;
		auto output_height = aspect_height * target_segments;

		if (output_width > ovi.base_width ||
		    output_height > ovi.base_height)
			continue;

		auto output_pixels = output_width * output_height;

		auto ratio =
			static_cast<float>(output_pixels) / requested_pixels;
		if (ratio < 0.9 || ratio > 1.1)
			continue;

		if (output_width % 4 != 0 ||
		    output_height % 2 !=
			    0) //libobs enforces multiple of 4 width and multiple of 2 height
			continue;

		if (output_width == requested_width &&
		    output_height == requested_height)
			return {static_cast<uint32_t>(output_width),
				static_cast<uint32_t>(requested_height)};

		blog(LOG_INFO,
		     "Scaled output resolution from %" PRIu64 "x%" PRIu64
		     " to %ux%u (base: %ux%u)",
		     requested_width, requested_height, output_width,
		     output_height, ovi.base_width, ovi.base_height);

		return {static_cast<uint32_t>(output_width),
			static_cast<uint32_t>(output_height)};
	}

	blog(LOG_WARNING,
	     "Failed to scale request resolution %" PRIu64 "x%" PRIu64
	     " to %ux%u",
	     requested_width, requested_height, ovi.base_width,
	     ovi.base_height);

	return {static_cast<uint32_t>(requested_width),
		static_cast<uint32_t>(requested_height)};
}

static void data_item_release(obs_data_item_t *item)
{
	obs_data_item_release(&item);
}

using OBSDataItemAutoRelease =
	OBSRefAutoRelease<obs_data_item_t *, data_item_release>;

static obs_scale_type load_gpu_scale_type(obs_data_t *encoder_config)
{
	const auto default_scale_type = OBS_SCALE_BICUBIC;

	OBSDataItemAutoRelease item =
		obs_data_item_byname(encoder_config, "gpu_scale_type");
	if (!item)
		return default_scale_type;

	switch (obs_data_item_gettype(item)) {
	case OBS_DATA_NUMBER: {
		auto val = obs_data_item_get_int(item);
		if (val < OBS_SCALE_POINT || val > OBS_SCALE_AREA) {
			blog(LOG_WARNING,
			     "load_gpu_scale_type: scale type out of range %lld (must be 1 <= value <= 5)",
			     val);
			break;
		}
		return static_cast<obs_scale_type>(val);
	}

	case OBS_DATA_STRING: {
		auto val = obs_data_item_get_string(item);
		if (strncmp(val, "OBS_SCALE_POINT", 16) == 0)
			return OBS_SCALE_POINT;
		if (strncmp(val, "OBS_SCALE_BICUBIC", 18) == 0)
			return OBS_SCALE_BICUBIC;
		if (strncmp(val, "OBS_SCALE_BILINEAR", 19) == 0)
			return OBS_SCALE_BILINEAR;
		if (strncmp(val, "OBS_SCALE_LANCZOS", 18) == 0)
			return OBS_SCALE_LANCZOS;
		if (strncmp(val, "OBS_SCALE_AREA", 15) == 0)
			return OBS_SCALE_AREA;
		blog(LOG_WARNING,
		     "load_gpu_scale_type: unknown scaling type: '%s'", val);
		break;
	}

	default:
		blog(LOG_WARNING, "load_gpu_scale_type: unknown data type: %d",
		     obs_data_item_gettype(item));
	}

	return default_scale_type;
}

static void adjust_video_encoder_scaling(const obs_video_info &ovi,
					 obs_encoder_t *video_encoder,
					 obs_data_t *encoder_config)
{
	uint64_t requested_width = obs_data_get_int(encoder_config, "width");
	uint64_t requested_height = obs_data_get_int(encoder_config, "height");

	if (ovi.output_width == requested_width ||
	    ovi.output_height == requested_height)
		return;

#if 1
	auto res = scale_resolution(ovi, requested_width, requested_height);
	obs_encoder_set_scaled_size(video_encoder, res.width, res.height);
#else
	obs_encoder_set_scaled_size(video_encoder,
				    static_cast<uint32_t>(requested_width),
				    static_cast<uint32_t>(requested_height));
#endif
	obs_encoder_set_gpu_scale_type(video_encoder,
				       load_gpu_scale_type(encoder_config));
}

static uint32_t closest_divisor(const obs_video_info &ovi,
				const media_frames_per_second &target_fps)
{
	auto target = (uint64_t)target_fps.numerator * ovi.fps_den;
	auto source = (uint64_t)ovi.fps_num * target_fps.denominator;
	return std::max(1u, static_cast<uint32_t>(source / target));
}

static void adjust_encoder_frame_rate_divisor(const obs_video_info &ovi,
					      obs_encoder_t *video_encoder,
					      obs_data_t *encoder_config,
					      const size_t encoder_index)
{
	media_frames_per_second requested_fps;
	const char *option = nullptr;
	if (!obs_data_get_frames_per_second(encoder_config, "framerate",
					    &requested_fps, &option)) {
		blog(LOG_WARNING, "`framerate` not specified for encoder %zu",
		     encoder_index);
		return;
	}

	if (ovi.fps_num == requested_fps.numerator &&
	    ovi.fps_den == requested_fps.denominator)
		return;

	auto divisor = closest_divisor(ovi, requested_fps);
	if (divisor <= 1)
		return;

	blog(LOG_INFO, "Setting frame rate divisor to %u for encoder %zu",
	     divisor, encoder_index);
	obs_encoder_set_frame_rate_divisor(video_encoder, divisor);
}

static const std::vector<const char *> &get_available_encoders()
{
	// encoders are currently only registered during startup, so keeping
	// a static vector around shouldn't be a problem
	static std::vector<const char *> available_encoders = [] {
		std::vector<const char *> available_encoders;
		for (size_t i = 0;; i++) {
			const char *id = nullptr;
			if (!obs_enum_encoder_types(i, &id))
				break;
			available_encoders.push_back(id);
		}
		return available_encoders;
	}();
	return available_encoders;
}

static bool encoder_available(const char *type)
{
	auto &encoders = get_available_encoders();
	return std::find_if(std::begin(encoders), std::end(encoders),
			    [=](const char *encoder) {
				    return strcmp(type, encoder) == 0;
			    }) != std::end(encoders);
}

static OBSEncoderAutoRelease create_video_encoder(DStr &name_buffer,
						  size_t encoder_index,
						  obs_data_t *encoder_config)
{
	auto encoder_type = obs_data_get_string(encoder_config, "type");
	if (!encoder_available(encoder_type)) {
		blog(LOG_ERROR, "Encoder type '%s' not available",
		     encoder_type);
		throw SimulcastError::warning(
			QTStr("FailedToStartStream.EncoderNotAvailable")
				.arg(encoder_type));
	}

	dstr_printf(name_buffer, "simulcast video encoder %zu", encoder_index);

	if (obs_data_has_user_value(encoder_config, "keyInt_sec") &&
	    !obs_data_has_user_value(encoder_config, "keyint_sec")) {
		blog(LOG_INFO,
		     "Fixing Go Live Config for encoder '%s': keyInt_sec -> keyint_sec",
		     name_buffer->array);
		obs_data_set_int(encoder_config, "keyint_sec",
				 obs_data_get_int(encoder_config,
						  "keyInt_sec"));
	}

	obs_data_set_bool(encoder_config, "disable_scenecut", true);

	OBSEncoderAutoRelease video_encoder = obs_video_encoder_create(
		encoder_type, name_buffer, encoder_config, nullptr);
	if (!video_encoder) {
		blog(LOG_ERROR, "failed to create video encoder '%s'",
		     name_buffer->array);
		throw SimulcastError::warning(
			QTStr("FailedToStartStream.FailedToCreateVideoEncoder")
				.arg(name_buffer->array, encoder_type));
	}
	obs_encoder_set_video(video_encoder, obs_get_video());

	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		blog(LOG_WARNING,
		     "Failed to get obs video info while creating encoder %zu",
		     encoder_index);
		throw SimulcastError::warning(
			QTStr("FailedToStartStream.FailedToGetOBSVideoInfo")
				.arg(name_buffer->array, encoder_type));
	}

	adjust_video_encoder_scaling(ovi, video_encoder, encoder_config);
	adjust_encoder_frame_rate_divisor(ovi, video_encoder, encoder_config,
					  encoder_index);

	return video_encoder;
}

static OBSEncoderAutoRelease create_audio_encoder()
{
	OBSEncoderAutoRelease audio_encoder = obs_audio_encoder_create(
		"ffmpeg_aac", "simulcast aac", nullptr, 0, nullptr);
	if (!audio_encoder) {
		blog(LOG_ERROR, "failed to create audio encoder");
		throw SimulcastError::warning(QTStr(
			"FailedToStartStream.FailedToCreateAudioEncoder"));
	}
	obs_encoder_set_audio(audio_encoder, obs_get_audio());
	return audio_encoder;
}

static OBSOutputAutoRelease
SetupOBSOutput(bool recording, obs_data_t *go_live_config,
	       std::vector<OBSEncoderAutoRelease> &video_encoders,
	       OBSEncoderAutoRelease &audio_encoder, bool use_ertmp_multitrack);
static void SetupSignalHandlers(bool recording, SimulcastOutput *self,
				obs_output_t *output);

struct OutputObjects {
	OBSOutputAutoRelease output;
	std::vector<OBSEncoderAutoRelease> video_encoders;
	OBSEncoderAutoRelease audio_encoder;
	OBSServiceAutoRelease simulcast_service;
};

void SimulcastOutput::PrepareStreaming(QWidget *parent, const QString &rtmp_url,
				       const QString &stream_key,
				       bool use_ertmp_multitrack)
{
	if (!berryessa_) {
		berryessa_ = std::make_unique<BerryessaSubmitter>(
			parent, "https://data.stats.live-video.net/");
		berryessa_->setAlwaysString("device_id", device_id());
		berryessa_->setAlwaysString("obs_session_id", obs_session_id());
	}

	auto attempt_start_time = GenerateStreamAttemptStartTime();
	OBSDataAutoRelease go_live_post;
	OBSDataAutoRelease go_live_config;
	quint64 download_time_elapsed = 0;

	try {
		go_live_post = constructGoLivePost(attempt_start_time,
						   std::nullopt, std::nullopt);

		go_live_config = DownloadGoLiveConfig(parent, GO_LIVE_API_URL,
						      go_live_post);
		if (!go_live_config)
			throw SimulcastError::warning(
				QTStr("FailedToStartStream.FallbackToDefault"));

		download_time_elapsed = attempt_start_time.MSecsElapsed();

		video_encoders_.clear();
		OBSEncoderAutoRelease audio_encoder = nullptr;
		auto output = SetupOBSOutput(false, go_live_config,
					     video_encoders_, audio_encoder,
					     use_ertmp_multitrack);
		if (!output)
			throw SimulcastError::warning(
				QTStr("FailedToStartStream.FallbackToDefault"));

		auto simulcast_service =
			create_service(device_id(), obs_session_id(),
				       go_live_config, rtmp_url, stream_key);
		if (!simulcast_service)
			throw SimulcastError::warning(
				QTStr("FailedToStartStream.FallbackToDefault"));

		obs_output_set_service(output, simulcast_service);

		SetupSignalHandlers(false, this, output);

		output_ = std::move(output);
		weak_output_ = obs_output_get_weak_output(output_);
		audio_encoder_ = std::move(audio_encoder);
		simulcast_service_ = std::move(simulcast_service);

		if (berryessa_) {
			send_start_event = [berryessa = berryessa_.get(),
					    attempt_start_time,
					    download_time_elapsed,
					    go_live_post =
						    OBSData{go_live_post},
					    go_live_config =
						    OBSData{go_live_config}](
						   bool success,
						   std::optional<int>
							   connect_time_ms) {
				auto start_streaming_returned =
					attempt_start_time.MSecsElapsed();
				if (!success) {

					auto event =
						MakeEvent_ivs_obs_stream_start_failed(
							go_live_post,
							go_live_config,
							attempt_start_time,
							download_time_elapsed,
							start_streaming_returned);
					submit_event(
						berryessa,
						"ivs_obs_stream_start_failed",
						event);
				} else {
					auto event =
						MakeEvent_ivs_obs_stream_start(
							go_live_post,
							go_live_config,
							attempt_start_time,
							download_time_elapsed,
							start_streaming_returned,
							connect_time_ms);

					const char *configId =
						obs_data_get_string(
							event, "config_id");
					if (configId) {
						// put the config_id on all events until the stream ends
						add_always_string(berryessa,
								  "config_id",
								  configId);
					} else if (berryessa) {
						berryessa->unsetAlways(
							"config_id");
					}

					add_always_string(
						berryessa,
						"stream_attempt_start_time",
						attempt_start_time.CStr());

					submit_event(berryessa,
						     "ivs_obs_stream_start",
						     event);
				}
			};
		}
	} catch (...) {
		auto start_streaming_returned =
			attempt_start_time.MSecsElapsed();
		auto event = MakeEvent_ivs_obs_stream_start_failed(
			go_live_post, go_live_config, attempt_start_time,
			download_time_elapsed, start_streaming_returned);
		submit_event(berryessa_.get(), "ivs_obs_stream_start_failed",
			     event);
		throw;
	}
}

signal_handler_t *SimulcastOutput::StreamingSignalHandler()
{
	return obs_output_get_signal_handler(output_);
}

void SimulcastOutput::StartedStreaming(QWidget *parent, bool success)
{
	if (!success) {
		if (send_start_event)
			send_start_event(false, std::nullopt);
		send_start_event = {};
		return;
	}

	if (send_start_event)
		send_start_event(true, ConnectTimeMs());

	send_start_event = {};

	if (berryessa_)
		berryessa_every_minute_ =
			std::make_unique<BerryessaEveryMinute>(
				parent, berryessa_.get(), VideoEncoders());
}

void SimulcastOutput::StopStreaming()
{
	if (output_)
		obs_output_stop(output_);

	submit_event(berryessa_.get(), "ivs_obs_stream_stop",
		     MakeEvent_ivs_obs_stream_stop());

	berryessa_every_minute_.reset(nullptr);

	if (berryessa_)
		berryessa_->unsetAlways("config_id");

	output_ = nullptr;
	video_encoders_.clear();
	audio_encoder_ = nullptr;

	streaming_ = false;
}

bool SimulcastOutput::IsStreaming() const
{
	return streaming_;
}

std::optional<int> SimulcastOutput::ConnectTimeMs() const
{
	if (!output_)
		return std::nullopt;

	return obs_output_get_connect_time_ms(output_);
}

bool SimulcastOutput::StartRecording(obs_data_t *go_live_config,
				     bool use_ertmp_multitrack)
{
	if (streaming_)
		return false;

	if (!go_live_config)
		return false;

	video_encoders_.clear();
	recording_output_ = SetupOBSOutput(true, go_live_config,
					   video_encoders_, audio_encoder_,
					   use_ertmp_multitrack);
	if (!recording_output_)
		return false;

	SetupSignalHandlers(true, this, recording_output_);

	weak_recording_output_ = obs_output_get_weak_output(recording_output_);
	if (!obs_output_start(recording_output_)) {
		blog(LOG_WARNING, "Failed to start recording");
		throw QString::asprintf(
			"Failed to start recording (obs_output_start returned false)");
	}

	blog(LOG_INFO, "starting recording");
	return true;
}

void SimulcastOutput::StopRecording()
{
	if (!recording_)
		return;

	if (recording_output_)
		obs_output_stop(recording_output_);

	recording_output_ = nullptr;
	video_encoders_.clear();
	audio_encoder_ = nullptr;

	recording_ = false;
}

const std::vector<OBSEncoderAutoRelease> &SimulcastOutput::VideoEncoders() const
{
	return video_encoders_;
}

static OBSOutputAutoRelease
SetupOBSOutput(bool recording, obs_data_t *go_live_config,
	       std::vector<OBSEncoderAutoRelease> &video_encoders,
	       OBSEncoderAutoRelease &audio_encoder, bool use_ermtp_multitrack)
{

	auto output = !recording
			      ? create_output(use_ermtp_multitrack)
			      : create_recording_output(use_ermtp_multitrack);

	OBSDataArrayAutoRelease encoder_configs =
		obs_data_get_array(go_live_config, "encoder_configurations");
	DStr video_encoder_name_buffer;
	obs_encoder_t *first_encoder = nullptr;
	const size_t num_encoder_configs =
		obs_data_array_count(encoder_configs);
	if (num_encoder_configs < 1)
		throw SimulcastError::warning(
			QTStr("FailedToStartStream.MissingEncoderConfigs"));

	for (size_t i = 0; i < num_encoder_configs; i++) {
		OBSDataAutoRelease encoder_config =
			obs_data_array_item(encoder_configs, i);
		auto encoder = create_video_encoder(video_encoder_name_buffer,
						    i, encoder_config);
		if (!encoder)
			return nullptr;

		if (!first_encoder)
			first_encoder = encoder;
		else
			obs_encoder_group_simulcast_encoder(first_encoder,
							    encoder);

		obs_output_set_video_encoder2(output, encoder, i);
		video_encoders.emplace_back(std::move(encoder));
	}

	audio_encoder = create_audio_encoder();
	obs_output_set_audio_encoder(output, audio_encoder, 0);

	return output;
}

void SetupSignalHandlers(bool recording, SimulcastOutput *self,
			 obs_output_t *output)
{
	auto handler = obs_output_get_signal_handler(output);

	signal_handler_connect(
		handler, "start",
		!recording ? StreamStartHandler : RecordingStartHandler, self);

	signal_handler_connect(
		handler, "stop",
		!recording ? StreamStopHandler : RecordingStopHandler, self);
}

void StreamStartHandler(void *arg, calldata_t * /* data */)
{
	auto self = static_cast<SimulcastOutput *>(arg);
	self->streaming_ = true;

	if (!self->stream_attempt_start_time_.has_value() || !self->berryessa_)
		return;

	auto event = MakeEvent_ivs_obs_stream_started(
		self->stream_attempt_start_time_->MSecsElapsed());
	self->berryessa_->submit("ivs_obs_stream_started", event);
}

void StreamStopHandler(void *arg, calldata_t * /* data */)
{
	auto self = static_cast<SimulcastOutput *>(arg);
	self->streaming_ = false;
	self->weak_output_ = nullptr;
}

void RecordingStartHandler(void *arg, calldata_t * /* data */)
{
	auto self = static_cast<SimulcastOutput *>(arg);
	self->recording_ = true;
}

void RecordingStopHandler(void *arg, calldata_t * /* data */)
{
	auto self = static_cast<SimulcastOutput *>(arg);
	self->recording_ = false;
	self->weak_recording_output_ = nullptr;
}

const ImmutableDateTime &SimulcastOutput::GenerateStreamAttemptStartTime()
{
	stream_attempt_start_time_.emplace(ImmutableDateTime::CurrentTimeUtc());
	return *stream_attempt_start_time_;
}
