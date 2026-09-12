/*
 * pipeline.c - builds the GStreamer/DeepStream graph.
 *
 *  [source] -> nvstreammux -> nvinfer -> nvtracker -> nvvideoconvert(RGBA)
 *           -> nvdsosd -> tee -> { udp | display | record }
 *
 * source is either nvarguscamerasrc (CSI) or uridecodebin.
 */
#include <string.h>
#include "app.h"

#define DS_LIB_DIR "/opt/nvidia/deepstream/deepstream-5.1/lib/"

static GstElement *mk(const char *factory, const char *name, GError **err)
{
	GstElement *e = gst_element_factory_make(factory, name);
	if (!e && err && !*err)
		g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_MISSING_PLUGIN,
			"could not create element '%s' (%s)", factory, name);
	return e;
}

static GstElement *caps_filter(const char *name, const char *caps_str, GError **err)
{
	GstElement *f = mk("capsfilter", name, err);
	if (!f) return NULL;
	GstCaps *caps = gst_caps_from_string(caps_str);
	g_object_set(f, "caps", caps, NULL);
	gst_caps_unref(caps);
	return f;
}

/* Link a src pad (from decodebin) to nvstreammux sink_0 once it appears. */
static void on_decode_pad_added(GstElement *decodebin, GstPad *pad, gpointer user_data)
{
	App *app = user_data;
	GstCaps *caps = gst_pad_get_current_caps(pad);
	if (!caps) caps = gst_pad_query_caps(pad, NULL);
	const GstStructure *s = gst_caps_get_structure(caps, 0);
	const gchar *name = gst_structure_get_name(s);
	if (g_str_has_prefix(name, "video/")) {
		GstCapsFeatures *feat = gst_caps_get_features(caps, 0);
		if (!gst_caps_features_contains(feat, "memory:NVMM"))
			g_printerr("warning: decoder output is not NVMM memory; performance will suffer\n");
		GstPad *sink = gst_element_get_request_pad(app->streammux, "sink_0");
		if (gst_pad_link(pad, sink) != GST_PAD_LINK_OK)
			g_printerr("failed to link decodebin to streammux\n");
		gst_object_unref(sink);
	}
	gst_caps_unref(caps);
}

/* Prefer the hardware decoder for files/RTSP. */
static void on_decode_child_added(GstChildProxy *proxy, GObject *obj, gchar *name, gpointer user_data)
{
	if (g_str_has_prefix(name, "decodebin"))
		g_signal_connect(obj, "child-added", G_CALLBACK(on_decode_child_added), user_data);
	if (g_str_has_prefix(name, "nvv4l2decoder"))
		g_object_set(obj, "enable-max-performance", TRUE, "drop-frame-interval", 0, "num-extra-surfaces", 0, NULL);
}

static gboolean add_source(App *app, GstBin *bin, GError **err)
{
	AppConfig *c = &app->cfg;
	if (c->source_uri) {
		GstElement *dec = mk("uridecodebin", "source", err);
		if (!dec) return FALSE;
		g_object_set(dec, "uri", c->source_uri, NULL);
		g_signal_connect(dec, "pad-added", G_CALLBACK(on_decode_pad_added), app);
		g_signal_connect(dec, "child-added", G_CALLBACK(on_decode_child_added), app);
		gst_bin_add(bin, dec);
		return TRUE;
	}

	GstElement *src  = mk("nvarguscamerasrc", "source", err);
	gchar *cs = g_strdup_printf("video/x-raw(memory:NVMM),width=%d,height=%d,framerate=%d/1,format=NV12",
		c->cap_width, c->cap_height, c->cap_fps);
	GstElement *caps = caps_filter("source-caps", cs, err);
	g_free(cs);
	GstElement *conv = mk("nvvideoconvert", "source-conv", err);
	GstElement *caps2 = caps_filter("source-caps2", "video/x-raw(memory:NVMM),format=NV12", err);
	if (!src || !caps || !conv || !caps2) return FALSE;
	g_object_set(src, "sensor-id", c->sensor_id, "wbmode", c->wbmode, NULL);
	if (c->flip_method) g_object_set(conv, "flip-method", c->flip_method, NULL);
	gst_bin_add_many(bin, src, caps, conv, caps2, NULL);
	if (!gst_element_link_many(src, caps, conv, caps2, NULL)) {
		g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION, "failed to link camera source chain");
		return FALSE;
	}
	GstPad *srcpad = gst_element_get_static_pad(caps2, "src");
	GstPad *sink = gst_element_get_request_pad(app->streammux, "sink_0");
	gboolean ok = gst_pad_link(srcpad, sink) == GST_PAD_LINK_OK;
	gst_object_unref(srcpad); gst_object_unref(sink);
	if (!ok) g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION, "failed to link camera to streammux");
	return ok;
}

static gboolean configure_tracker(App *app, GstElement *tracker, GError **err)
{
	AppConfig *c = &app->cfg;
	const char *lib;
	if (g_strcmp0(c->tracker, "nvdcf") == 0)      lib = DS_LIB_DIR "libnvds_nvdcf.so";
	else if (g_strcmp0(c->tracker, "klt") == 0)   lib = DS_LIB_DIR "libnvds_mot_klt.so";
	else if (g_strcmp0(c->tracker, "iou") == 0)   lib = DS_LIB_DIR "libnvds_mot_iou.so";
	else {
		g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "unknown tracker '%s' (nvdcf|klt|iou)", c->tracker);
		return FALSE;
	}
	g_object_set(tracker,
		"tracker-width", c->tracker_width, "tracker-height", c->tracker_height,
		"gpu-id", 0, "ll-lib-file", lib, "enable-batch-process", TRUE, NULL);
	if (c->tracker_config && g_strcmp0(c->tracker, "klt") != 0)
		g_object_set(tracker, "ll-config-file", c->tracker_config, NULL);
	return TRUE;
}

/* tee -> queue -> [elements...] ; returns FALSE on link failure */
static gboolean add_branch(GstBin *bin, GstElement *tee, GError **err, GstElement *first, ...)
{
	GstElement *q = mk("queue", NULL, err);
	if (!q) return FALSE;
	gst_bin_add(bin, q);
	GstElement *prev = q;
	va_list ap; va_start(ap, first);
	for (GstElement *e = first; e; e = va_arg(ap, GstElement *)) {
		gst_bin_add(bin, e);
		if (!gst_element_link(prev, e)) {
			va_end(ap);
			g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION, "failed to link %s -> %s",
				GST_ELEMENT_NAME(prev), GST_ELEMENT_NAME(e));
			return FALSE;
		}
		prev = e;
	}
	va_end(ap);
	GstPad *tp = gst_element_get_request_pad(tee, "src_%u");
	GstPad *qp = gst_element_get_static_pad(q, "sink");
	gboolean ok = gst_pad_link(tp, qp) == GST_PAD_LINK_OK;
	gst_object_unref(tp); gst_object_unref(qp);
	if (!ok) g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION, "failed to link tee branch");
	return ok;
}

/* H.264 encoder chain shared by udp and record branches. */
static GstElement *make_encoder(App *app, const char *name, GError **err)
{
	GstElement *enc = mk("nvv4l2h264enc", name, err);
	if (!enc) return NULL;
	g_object_set(enc, "bitrate", app->cfg.bitrate_kbps * 1000, "insert-sps-pps", TRUE,
		"iframeinterval", app->cfg.cap_fps, "idrinterval", app->cfg.cap_fps,
		"preset-level", 1, "maxperf-enable", TRUE, NULL);
	return enc;
}

gboolean app_build_pipeline(App *app, GError **err)
{
	AppConfig *c = &app->cfg;
	app->pipeline = gst_pipeline_new("house-tracker");
	GstBin *bin = GST_BIN(app->pipeline);

	app->streammux = mk("nvstreammux", "streammux", err);
	GstElement *pgie    = mk("nvinfer", "primary-infer", err);
	GstElement *tracker = mk("nvtracker", "tracker", err);
	GstElement *conv    = mk("nvvideoconvert", "osd-conv", err);
	GstElement *capsrgba= caps_filter("osd-caps", "video/x-raw(memory:NVMM),format=RGBA", err);
	GstElement *osd     = mk("nvdsosd", "osd", err);
	GstElement *tee     = mk("tee", "tee", err);
	if (!app->streammux || !pgie || !tracker || !conv || !capsrgba || !osd || !tee) return FALSE;

	g_object_set(app->streammux, "batch-size", 1, "width", c->cap_width, "height", c->cap_height,
		"live-source", c->source_uri ? FALSE : TRUE, "batched-push-timeout", 40000, NULL);
	g_object_set(pgie, "config-file-path", c->infer_config, NULL);
	if (!configure_tracker(app, tracker, err)) return FALSE;
	g_object_set(osd, "display-clock", FALSE, "display-text", TRUE, "display-bbox", TRUE, NULL);

	gst_bin_add_many(bin, app->streammux, pgie, tracker, conv, capsrgba, osd, tee, NULL);
	if (!add_source(app, bin, err)) return FALSE;
	if (!gst_element_link_many(app->streammux, pgie, tracker, conv, capsrgba, osd, tee, NULL)) {
		g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION, "failed to link inference chain");
		return FALSE;
	}

	/* metadata probe on the OSD sink pad: this is where tracking events come from */
	GstPad *osd_sink = gst_element_get_static_pad(osd, "sink");
	gst_pad_add_probe(osd_sink, GST_PAD_PROBE_TYPE_BUFFER, events_osd_probe, app, NULL);
	gst_object_unref(osd_sink);

	gboolean any = FALSE;
	if (c->udp_target) {
		gchar **hp = g_strsplit(c->udp_target, ":", 2);
		if (!hp[0] || !hp[1]) {
			g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "--udp needs HOST:PORT");
			return FALSE;
		}
		GstElement *cv = mk("nvvideoconvert", "udp-conv", err);
		GstElement *cf = caps_filter("udp-caps", "video/x-raw(memory:NVMM),format=NV12", err);
		GstElement *enc = make_encoder(app, "udp-enc", err);
		GstElement *parse = mk("h264parse", "udp-parse", err);
		GstElement *mux = mk("mpegtsmux", "udp-mux", err);
		GstElement *sink = mk("udpsink", "udp-sink", err);
		if (!cv || !cf || !enc || !parse || !mux || !sink) return FALSE;
		g_object_set(sink, "host", hp[0], "port", atoi(hp[1]), "sync", FALSE, "async", FALSE, NULL);
		g_strfreev(hp);
		if (!add_branch(bin, tee, err, cv, cf, enc, parse, mux, sink, NULL)) return FALSE;
		any = TRUE;
	}
	if (c->record_path) {
		GstElement *cv = mk("nvvideoconvert", "rec-conv", err);
		GstElement *cf = caps_filter("rec-caps", "video/x-raw(memory:NVMM),format=NV12", err);
		GstElement *enc = make_encoder(app, "rec-enc", err);
		GstElement *parse = mk("h264parse", "rec-parse", err);
		GstElement *mux = mk("qtmux", "rec-mux", err);
		GstElement *sink = mk("filesink", "rec-sink", err);
		if (!cv || !cf || !enc || !parse || !mux || !sink) return FALSE;
		g_object_set(sink, "location", c->record_path, NULL);
		if (!add_branch(bin, tee, err, cv, cf, enc, parse, mux, sink, NULL)) return FALSE;
		any = TRUE;
	}
	if (c->display) {
		GstElement *tr = mk("nvegltransform", "disp-transform", err);
		GstElement *sink = mk("nveglglessink", "disp-sink", err);
		if (!tr || !sink) return FALSE;
		g_object_set(sink, "sync", FALSE, NULL);
		if (!add_branch(bin, tee, err, tr, sink, NULL)) return FALSE;
		any = TRUE;
	}
	if (!any) {
		GstElement *sink = mk("fakesink", "null-sink", err);
		if (!sink) return FALSE;
		g_object_set(sink, "sync", FALSE, NULL);
		if (!add_branch(bin, tee, err, sink, NULL)) return FALSE;
	}
	return TRUE;
}
