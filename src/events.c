/*
 * events.c - turns DeepStream object metadata into tracking events.
 *
 * Runs in the streaming thread via a pad probe on nvdsosd's sink pad.
 * Keeps one Track per tracker object id and emits JSON lines:
 *   {"event":"appeared", ...}  first frame an id is seen
 *   {"event":"lost", ...}      id not seen for --lost-frames frames
 *   {"event":"summary", ...}   every --summary-interval seconds (main loop)
 * and draws an FPS / active-object banner into the OSD.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "gstnvdsmeta.h"
#include "app.h"

typedef struct {
	guint64  id;
	gint     class_id;
	gchar    label[64];
	gint64   first_us, last_us;
	gint     first_frame, last_frame;
	guint    hits;
	gfloat   conf;
	gfloat   left, top, width, height;
} Track;

static gchar *iso_now(gint64 us)
{
	GDateTime *dt = g_date_time_new_from_unix_utc(us / G_USEC_PER_SEC);
	gchar *base = g_date_time_format(dt, "%Y-%m-%dT%H:%M:%S");
	gchar *s = g_strdup_printf("%s.%03dZ", base, (gint)((us % G_USEC_PER_SEC) / 1000));
	g_free(base); g_date_time_unref(dt);
	return s;
}

static void emit(App *app, const gchar *json)
{
	fputs(json, app->events_fp);
	fputc('\n', app->events_fp);
	fflush(app->events_fp);
}

/* escape the few characters a COCO label could contain */
static const gchar *safe_label(const gchar *l, gchar *buf, gsize n)
{
	gsize j = 0;
	for (gsize i = 0; l[i] && j + 2 < n; i++) {
		if (l[i] == '"' || l[i] == '\\') buf[j++] = '\\';
		buf[j++] = l[i];
	}
	buf[j] = 0;
	return buf;
}

void events_init(App *app)
{
	app->tracks = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, g_free);
	app->events_fp = stdout;
	if (app->cfg.events_path) {
		FILE *f = fopen(app->cfg.events_path, "a");
		if (f) app->events_fp = f;
		else g_printerr("could not open %s for events, using stdout\n", app->cfg.events_path);
	}
	app->fps_window_start_us = g_get_monotonic_time();
	app->last_frame_num = -1;
}

void events_shutdown(App *app)
{
	g_mutex_lock(&app->lock);
	GHashTableIter it; gpointer k, v;
	gint64 now = g_get_real_time();
	g_hash_table_iter_init(&it, app->tracks);
	while (g_hash_table_iter_next(&it, &k, &v)) {
		Track *t = v; gchar lb[80]; gchar *ts = iso_now(now);
		gchar *j = g_strdup_printf("{\"event\":\"lost\",\"ts\":\"%s\",\"frame\":%d,\"id\":%" G_GUINT64_FORMAT
			",\"class\":\"%s\",\"frames_tracked\":%u,\"duration_s\":%.2f,\"reason\":\"shutdown\"}",
			ts, t->last_frame, t->id, safe_label(t->label, lb, sizeof lb), t->hits, (t->last_us - t->first_us) / 1e6);
		emit(app, j); g_free(j); g_free(ts);
	}
	g_hash_table_remove_all(app->tracks);
	g_mutex_unlock(&app->lock);
	if (app->events_fp && app->events_fp != stdout) fclose(app->events_fp);
	g_hash_table_unref(app->tracks);
}

/* Called from the main loop; summarises what is currently tracked. */
gboolean events_summary_tick(gpointer user_data)
{
	App *app = user_data;
	GHashTable *counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_mutex_lock(&app->lock);
	GHashTableIter it; gpointer k, v;
	g_hash_table_iter_init(&it, app->tracks);
	while (g_hash_table_iter_next(&it, &k, &v)) {
		Track *t = v;
		gpointer cur = g_hash_table_lookup(counts, t->label);
		g_hash_table_insert(counts, g_strdup(t->label), GINT_TO_POINTER(GPOINTER_TO_INT(cur) + 1));
	}
	gdouble fps = app->fps; guint64 frames = app->frames_seen; gint fnum = app->last_frame_num;
	g_mutex_unlock(&app->lock);

	GString *s = g_string_new(NULL);
	gchar *ts = iso_now(g_get_real_time());
	g_string_append_printf(s, "{\"event\":\"summary\",\"ts\":\"%s\",\"frame\":%d,\"frames_total\":%" G_GUINT64_FORMAT
		",\"fps\":%.1f,\"active\":{", ts, fnum, frames, fps);
	gboolean first = TRUE;
	g_hash_table_iter_init(&it, counts);
	while (g_hash_table_iter_next(&it, &k, &v)) {
		gchar lb[80];
		g_string_append_printf(s, "%s\"%s\":%d", first ? "" : ",", safe_label(k, lb, sizeof lb), GPOINTER_TO_INT(v));
		first = FALSE;
	}
	g_string_append(s, "}}");
	emit(app, s->str);
	g_string_free(s, TRUE); g_free(ts);
	g_hash_table_unref(counts);
	return G_SOURCE_CONTINUE;
}

static void draw_banner(App *app, NvDsBatchMeta *batch_meta, NvDsFrameMeta *frame_meta, guint n_obj)
{
	NvDsDisplayMeta *dm = nvds_acquire_display_meta_from_pool(batch_meta);
	NvOSD_TextParams *tp = &dm->text_params[0];
	dm->num_labels = 1;
	tp->display_text = g_strdup_printf("%.1f fps | %u objects | %u tracked", app->fps, n_obj,
		g_hash_table_size(app->tracks));
	tp->x_offset = 10; tp->y_offset = 12;
	tp->font_params.font_name = "Serif";
	tp->font_params.font_size = 12;
	tp->font_params.font_color = (NvOSD_ColorParams){ 1.0, 1.0, 1.0, 1.0 };
	tp->set_bg_clr = 1;
	tp->text_bg_clr = (NvOSD_ColorParams){ 0.0, 0.0, 0.0, 0.6 };
	nvds_add_display_meta_to_frame(frame_meta, dm);
}

GstPadProbeReturn events_osd_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
	App *app = user_data;
	GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
	NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
	if (!batch_meta) return GST_PAD_PROBE_OK;

	gint64 now_real = g_get_real_time();
	gint64 now_mono = g_get_monotonic_time();

	g_mutex_lock(&app->lock);
	for (NvDsMetaList *lf = batch_meta->frame_meta_list; lf; lf = lf->next) {
		NvDsFrameMeta *fm = (NvDsFrameMeta *) lf->data;
		gint frame = fm->frame_num;
		guint n_obj = 0;

		for (NvDsMetaList *lo = fm->obj_meta_list; lo; lo = lo->next) {
			NvDsObjectMeta *om = (NvDsObjectMeta *) lo->data;
			n_obj++;
			if (om->object_id == UNTRACKED_OBJECT_ID) continue;
			guint64 id = om->object_id;
			Track *t = g_hash_table_lookup(app->tracks, &id);
			if (!t) {
				t = g_new0(Track, 1);
				t->id = id; t->class_id = om->class_id;
				g_strlcpy(t->label, om->obj_label[0] ? om->obj_label : "unknown", sizeof t->label);
				t->first_us = now_real; t->first_frame = frame;
				g_hash_table_insert(app->tracks, &t->id, t);
				gchar lb[80]; gchar *ts = iso_now(now_real);
				gchar *j = g_strdup_printf("{\"event\":\"appeared\",\"ts\":\"%s\",\"frame\":%d,\"id\":%" G_GUINT64_FORMAT
					",\"class\":\"%s\",\"conf\":%.2f,\"bbox\":[%.0f,%.0f,%.0f,%.0f]}",
					ts, frame, id, safe_label(t->label, lb, sizeof lb), om->confidence,
					om->rect_params.left, om->rect_params.top, om->rect_params.width, om->rect_params.height);
				emit(app, j); g_free(j); g_free(ts);
			}
			t->last_us = now_real; t->last_frame = frame; t->hits++;
			t->conf = om->confidence;
			t->left = om->rect_params.left; t->top = om->rect_params.top;
			t->width = om->rect_params.width; t->height = om->rect_params.height;
			/* show "label #id" on the box */
			if (om->text_params.display_text) {
				g_free(om->text_params.display_text);
				om->text_params.display_text = g_strdup_printf("%s #%" G_GUINT64_FORMAT, t->label, id);
			}
		}

		/* expire tracks not seen recently */
		GHashTableIter it; gpointer k, v;
		g_hash_table_iter_init(&it, app->tracks);
		while (g_hash_table_iter_next(&it, &k, &v)) {
			Track *t = v;
			if (frame - t->last_frame > app->cfg.lost_frames) {
				gchar lb[80]; gchar *ts = iso_now(now_real);
				gchar *j = g_strdup_printf("{\"event\":\"lost\",\"ts\":\"%s\",\"frame\":%d,\"id\":%" G_GUINT64_FORMAT
					",\"class\":\"%s\",\"frames_tracked\":%u,\"duration_s\":%.2f,\"last_bbox\":[%.0f,%.0f,%.0f,%.0f]}",
					ts, frame, t->id, safe_label(t->label, lb, sizeof lb), t->hits,
					(t->last_us - t->first_us) / 1e6, t->left, t->top, t->width, t->height);
				emit(app, j); g_free(j); g_free(ts);
				g_hash_table_iter_remove(&it);
			}
		}

		/* fps: rolling one-second window */
		app->frames_seen++; app->fps_window_frames++; app->last_frame_num = frame;
		gint64 dt = now_mono - app->fps_window_start_us;
		if (dt >= G_USEC_PER_SEC) {
			app->fps = app->fps_window_frames * 1e6 / dt;
			app->fps_window_frames = 0; app->fps_window_start_us = now_mono;
		}
		if (!app->cfg.no_osd_text) draw_banner(app, batch_meta, fm, n_obj);
	}
	g_mutex_unlock(&app->lock);
	return GST_PAD_PROBE_OK;
}
