/*
 * deepstream-house-tracker
 *
 * Live object detection + tracking on a Jetson with DeepStream 5.1:
 *   CSI camera (nvarguscamerasrc) or any URI
 *     -> nvstreammux -> nvinfer (YOLOv4-tiny, TensorRT FP16 engine)
 *     -> nvtracker (NvDCF / KLT / IOU) -> nvdsosd
 *     -> H.264 over UDP (MPEG-TS), optional on-screen display and MP4 record
 *   plus JSON-lines events (object appeared / lost / periodic summary).
 *
 * Copyright (c) 2026 Nick Stassen. MIT License.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <glib-unix.h>
#include "app.h"

static App *g_app;

static gboolean on_sigint(gpointer data)
{
	App *app = data;
	g_printerr("\nSIGINT: sending EOS\n");
	gst_element_send_event(app->pipeline, gst_event_new_eos());
	/* Fallback in case EOS never arrives (live sources sometimes stall). */
	g_timeout_add_seconds(3, (GSourceFunc) g_main_loop_quit, app->loop);
	return G_SOURCE_REMOVE;
}

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data)
{
	App *app = data;
	switch (GST_MESSAGE_TYPE(msg)) {
	case GST_MESSAGE_EOS:
		g_printerr("EOS\n");
		g_main_loop_quit(app->loop);
		break;
	case GST_MESSAGE_WARNING: {
		gchar *dbg = NULL; GError *e = NULL;
		gst_message_parse_warning(msg, &e, &dbg);
		g_printerr("WARNING from %s: %s\n", GST_OBJECT_NAME(msg->src), e->message);
		if (dbg && app->cfg.verbose) g_printerr("  %s\n", dbg);
		g_error_free(e); g_free(dbg);
		break;
	}
	case GST_MESSAGE_ERROR: {
		gchar *dbg = NULL; GError *e = NULL;
		gst_message_parse_error(msg, &e, &dbg);
		g_printerr("ERROR from %s: %s\n", GST_OBJECT_NAME(msg->src), e->message);
		if (dbg) g_printerr("  %s\n", dbg);
		g_error_free(e); g_free(dbg);
		g_main_loop_quit(app->loop);
		break;
	}
	case GST_MESSAGE_STATE_CHANGED:
		if (app->cfg.verbose && GST_MESSAGE_SRC(msg) == GST_OBJECT(app->pipeline)) {
			GstState o, n;
			gst_message_parse_state_changed(msg, &o, &n, NULL);
			g_printerr("pipeline: %s -> %s\n", gst_element_state_get_name(o), gst_element_state_get_name(n));
		}
		break;
	default:
		break;
	}
	return TRUE;
}

/* Default config paths live next to the binary: bin/../configs/... */
static gchar *default_path(const char *argv0, const char *rel)
{
	gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	gchar *bindir = g_path_get_dirname(exe ? exe : argv0);
	gchar *root = g_path_get_dirname(bindir);
	gchar *p = g_build_filename(root, rel, NULL);
	g_free(exe); g_free(bindir); g_free(root);
	return p;
}

int main(int argc, char *argv[])
{
	App app;
	memset(&app, 0, sizeof app);
	g_app = &app;
	AppConfig *c = &app.cfg;
	c->sensor_id = 0; c->cap_width = 1280; c->cap_height = 720; c->cap_fps = 30;
	c->flip_method = 0; c->wbmode = 1;
	c->tracker = g_strdup("nvdcf"); c->tracker_width = 480; c->tracker_height = 288;
	c->bitrate_kbps = 4000; c->lost_frames = 30; c->summary_interval = 10;

	GOptionEntry entries[] = {
		{ "sensor-id", 's', 0, G_OPTION_ARG_INT, &c->sensor_id, "CSI camera index (default 0)", "N" },
		{ "width", 0, 0, G_OPTION_ARG_INT, &c->cap_width, "Capture width (default 1280)", "W" },
		{ "height", 0, 0, G_OPTION_ARG_INT, &c->cap_height, "Capture height (default 720)", "H" },
		{ "fps", 0, 0, G_OPTION_ARG_INT, &c->cap_fps, "Capture frame rate (default 30)", "N" },
		{ "flip", 0, 0, G_OPTION_ARG_INT, &c->flip_method, "nvvidconv flip-method 0..7 (2 = 180 deg)", "N" },
		{ "wbmode", 0, 0, G_OPTION_ARG_INT, &c->wbmode, "nvarguscamerasrc white balance mode (default 1 = auto)", "N" },
		{ "source", 'i', 0, G_OPTION_ARG_STRING, &c->source_uri, "Use a URI (file:///x.mp4, rtsp://...) instead of the CSI camera", "URI" },
		{ "infer-config", 'c', 0, G_OPTION_ARG_FILENAME, &c->infer_config, "nvinfer config (default configs/config_infer_primary_yolov4-tiny.txt)", "PATH" },
		{ "tracker", 't', 0, G_OPTION_ARG_STRING, &c->tracker, "nvdcf | klt | iou (default nvdcf)", "NAME" },
		{ "tracker-config", 0, 0, G_OPTION_ARG_FILENAME, &c->tracker_config, "Low-level tracker config (default configs/tracker_nvdcf.yml for nvdcf)", "PATH" },
		{ "tracker-width", 0, 0, G_OPTION_ARG_INT, &c->tracker_width, "Tracker frame width, multiple of 32 (default 480)", "W" },
		{ "tracker-height", 0, 0, G_OPTION_ARG_INT, &c->tracker_height, "Tracker frame height, multiple of 32 (default 288)", "H" },
		{ "udp", 'u', 0, G_OPTION_ARG_STRING, &c->udp_target, "Stream annotated H.264/MPEG-TS to HOST:PORT", "HOST:PORT" },
		{ "bitrate", 'b', 0, G_OPTION_ARG_INT, &c->bitrate_kbps, "H.264 bitrate in kbit/s (default 4000)", "KBPS" },
		{ "display", 'd', 0, G_OPTION_ARG_NONE, &c->display, "Also show on the local display (nveglglessink)", NULL },
		{ "record", 'r', 0, G_OPTION_ARG_FILENAME, &c->record_path, "Also record annotated video to an MP4 file", "PATH" },
		{ "events", 'e', 0, G_OPTION_ARG_FILENAME, &c->events_path, "Write JSON-lines events here (default stdout)", "PATH" },
		{ "no-osd-text", 0, 0, G_OPTION_ARG_NONE, &c->no_osd_text, "Do not draw the FPS/object-count banner", NULL },
		{ "lost-frames", 0, 0, G_OPTION_ARG_INT, &c->lost_frames, "Frames without a detection before a track is reported lost (default 30)", "N" },
		{ "summary-interval", 0, 0, G_OPTION_ARG_INT, &c->summary_interval, "Seconds between summary events, 0 = off (default 10)", "SEC" },
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &c->verbose, "Chatty bus/state logging", NULL },
		{ NULL }
	};

	GError *err = NULL;
	GOptionContext *ctx = g_option_context_new("- DeepStream household object tracker");
	g_option_context_add_main_entries(ctx, entries, NULL);
	g_option_context_add_group(ctx, gst_init_get_option_group());
	if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
		g_printerr("%s\n", err->message);
		return 1;
	}
	g_option_context_free(ctx);

	if (!c->infer_config)
		c->infer_config = default_path(argv[0], "configs/config_infer_primary_yolov4-tiny.txt");
	if (!c->tracker_config && g_strcmp0(c->tracker, "nvdcf") == 0)
		c->tracker_config = default_path(argv[0], "configs/tracker_nvdcf.yml");
	if (!c->udp_target && !c->display && !c->record_path)
		g_printerr("note: no --udp/--display/--record given; running headless, events only\n");

	app.loop = g_main_loop_new(NULL, FALSE);
	g_mutex_init(&app.lock);
	events_init(&app);

	if (!app_build_pipeline(&app, &err)) {
		g_printerr("pipeline build failed: %s\n", err ? err->message : "unknown");
		return 2;
	}

	GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(app.pipeline));
	gst_bus_add_watch(bus, bus_call, &app);
	gst_object_unref(bus);
	g_unix_signal_add(SIGINT, on_sigint, &app);
	g_unix_signal_add(SIGTERM, on_sigint, &app);
	if (c->summary_interval > 0)
		g_timeout_add_seconds(c->summary_interval, events_summary_tick, &app);

	g_printerr("starting pipeline (first run builds the TensorRT engine, this can take several minutes)\n");
	if (gst_element_set_state(app.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		g_printerr("failed to set pipeline to PLAYING\n");
		return 3;
	}
	g_main_loop_run(app.loop);

	gst_element_set_state(app.pipeline, GST_STATE_NULL);
	events_shutdown(&app);
	gst_object_unref(app.pipeline);
	g_main_loop_unref(app.loop);
	return 0;
}
