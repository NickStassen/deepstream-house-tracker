/* app.h - shared types for deepstream-house-tracker */
#ifndef HOUSE_TRACKER_APP_H
#define HOUSE_TRACKER_APP_H

#include <stdio.h>
#include <gst/gst.h>
#include <glib.h>

typedef struct {
	/* source */
	gint      sensor_id;
	gint      cap_width, cap_height, cap_fps;
	gint      flip_method;
	gint      wbmode;
	gint      tnr_mode;        /* nvarguscamerasrc tnr-mode 0 off, 1 fast, 2 high quality */
	gdouble   tnr_strength;    /* -1..1 */
	gint      ee_mode;         /* edge enhancement 0 off, 1 fast, 2 high quality */
	gdouble   max_gain;        /* analog gain ceiling, 0 = sensor default */
	gdouble   max_exposure_ms; /* exposure ceiling, 0 = frame duration */
	gchar    *source_uri;      /* if set, use uridecodebin instead of CSI */
	/* inference / tracking */
	gchar    *infer_config;
	gchar    *tracker;         /* nvdcf | klt | iou */
	gchar    *tracker_config;
	gint      tracker_width, tracker_height;
	/* outputs */
	gchar    *udp_target;      /* host:port for MPEG-TS/H.264 */
	gint      bitrate_kbps;
	gboolean  display;
	gchar    *record_path;
	gchar    *events_path;     /* JSON lines; NULL = stdout */
	gboolean  no_osd_text;
	gint      lost_frames;
	gint      summary_interval;
	gboolean  verbose;
} AppConfig;

typedef struct {
	AppConfig     cfg;
	GMainLoop    *loop;
	GstElement   *pipeline;
	GstElement   *streammux;
	/* tracker bookkeeping (see events.c) */
	GMutex        lock;
	GHashTable   *tracks;       /* object_id -> Track* */
	FILE         *events_fp;
	guint64       frames_seen;
	gint64        fps_window_start_us;
	guint         fps_window_frames;
	gdouble       fps;
	gint          last_frame_num;
} App;

/* pipeline.c */
gboolean app_build_pipeline(App *app, GError **err);

/* events.c */
void     events_init(App *app);
void     events_shutdown(App *app);
GstPadProbeReturn events_osd_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);
gboolean events_summary_tick(gpointer user_data);

#endif
