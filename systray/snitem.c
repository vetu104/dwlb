#include "snitem.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>

#include "sndbusmenu.h"


struct _SnItem
{
	GtkWidget parent_instance;

	char* busname;
	char* busobj;

	GDBusProxy* proxy;
	char* iconname;
	GVariant* iconpixmap;
	SnDbusmenu* dbusmenu;

	GtkWidget* image;
	GtkWidget* popovermenu;
	GMenu* init_menu;
	GSList* cachedicons;

	int iconsize;
	gboolean ready;
	gboolean exiting;
	gboolean menu_visible;

	ulong popup_sig_id;
};

G_DEFINE_FINAL_TYPE(SnItem, sn_item, GTK_TYPE_WIDGET)

enum
{
	PROP_BUSNAME = 1,
	PROP_BUSOBJ,
	PROP_ICONSIZE,
	PROP_PROXY,
	PROP_DBUSMENU,
	PROP_MENUVISIBLE,
	N_PROPERTIES
};

enum
{
	RIGHTCLICK,
	LAST_SIGNAL
};

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };
static uint signals[LAST_SIGNAL];

typedef struct {
	GVariant* iconpixmap;
	char* iconname;
	GdkPaintable* icondata;
} CachedIcon;

static void	sn_item_constructed	(GObject *obj);
static void	sn_item_dispose		(GObject *obj);
static void	sn_item_finalize	(GObject *obj);

static void	sn_item_size_allocate	(GtkWidget *widget,
					int width,
					int height,
					int baseline);

static void	sn_item_measure		(GtkWidget *widget,
					GtkOrientation orientation,
					int for_size,
					int *minimum,
					int *natural,
					int *minimum_baseline,
					int *natural_baseline);


static void
argb_to_rgba(int32_t width, int32_t height, unsigned char *icon_data)
{
	// Icon data is ARGB, gdk textures are RGBA. Flip the channels
	// Shamelessly copied from Waybar
	for (int32_t i = 0; i < 4 * width * height; i += 4) {
	        unsigned char alpha = icon_data[i];
	        icon_data[i] = icon_data[i + 1];
	        icon_data[i + 1] = icon_data[i + 2];
	        icon_data[i + 2] = icon_data[i + 3];
	        icon_data[i + 3] = alpha;
	}
}

static void
cachedicons_free(void *data)
{
	CachedIcon *cicon = (CachedIcon*)data;
	g_free(cicon->iconname);
	if (cicon->iconpixmap)
		g_variant_unref(cicon->iconpixmap);
	if (cicon->icondata)
		g_object_unref(cicon->icondata);
	g_free(cicon);
}

static void
pixbuf_destroy(unsigned char *pixeld, void *data)
{
	g_free(pixeld);
}

static GVariant*
select_icon_by_size(GVariant *icondata_v, int32_t target_icon_size)
{
	// Apps broadcast icons as variant a(iiay)
	// Meaning array of tuples, tuple representing an icon
	// first 2 members ii in each tuple are width and height
	// We iterate the array and pick the icon size closest to
	// the target based on its width and save the index
	GVariantIter iter;
	int selected_index = 0;
	int current_index = 0;
	int32_t diff = INT32_MAX;
	GVariant *child;
	g_variant_iter_init(&iter, icondata_v);
	while ((child = g_variant_iter_next_value(&iter))) {
		int32_t curwidth;
		g_variant_get_child(child, 0, "i", &curwidth);
		int32_t curdiff;
		if (curwidth > target_icon_size)
			curdiff = curwidth - target_icon_size;
		else
			curdiff = target_icon_size - curwidth;

		if (curdiff < diff)
			selected_index = current_index;

		current_index = current_index + 1;
		g_variant_unref(child);
	}

	GVariant *iconpixmap_v = g_variant_get_child_value(icondata_v,
	                                                   (size_t)selected_index);

	return iconpixmap_v;
}

static GdkPaintable*
get_paintable_from_data(GVariant *iconpixmap_v, int32_t iconsize)
{
	GdkPaintable *paintable;
	GVariantIter iter;

	int32_t width;
	int32_t height;
	GVariant *icon_data_v;

	g_variant_iter_init(&iter, iconpixmap_v);

	g_variant_iter_next(&iter, "i", &width);
	g_variant_iter_next(&iter, "i", &height);
	icon_data_v = g_variant_iter_next_value(&iter);

	size_t size = g_variant_get_size(icon_data_v);
	const void *icon_data_dup = g_variant_get_data(icon_data_v);

	unsigned char *icon_data = g_memdup2(icon_data_dup, size);
	argb_to_rgba(width, height, icon_data);

	int32_t padding = size / height - 4 * width;
	int32_t rowstride = 4 * width + padding;

	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(icon_data,
	                                             GDK_COLORSPACE_RGB,
	                                             TRUE,
	                                             8,
	                                             width,
	                                             height,
	                                             rowstride,
	                                             (GdkPixbufDestroyNotify)pixbuf_destroy,
	                                             NULL);

	GdkTexture *texture = gdk_texture_new_for_pixbuf(pixbuf);
	paintable = GDK_PAINTABLE(texture);

	g_object_unref(pixbuf);
	g_variant_unref(icon_data_v);

	return paintable;
}

static GdkPaintable*
get_paintable_from_name(const char *iconname, int32_t iconsize)
{
	GdkPaintable *paintable = NULL;
	GtkIconPaintable *icon;

	GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
	icon = gtk_icon_theme_lookup_icon(theme,
	                                  iconname,
	                                  NULL,  // const char **fallbacks
	                                  iconsize,
	                                  1,
	                                  GTK_TEXT_DIR_LTR,
	                                  0);  // GtkIconLookupFlags
	paintable = GDK_PAINTABLE(icon);

	return paintable;
}

static int
find_cached_name(CachedIcon *cicon, const char *name)
{

	return strcmp(cicon->iconname, name);
}

static void
sn_item_proxy_new_iconname_handler(GObject *obj, GAsyncResult *res, void *data)
{
	SnItem *self = SN_ITEM(data);
	GDBusProxy *proxy = G_DBUS_PROXY(obj);

	GError *err = NULL;
	GVariant *retvariant = g_dbus_proxy_call_finish(proxy, res, &err);
	// (v)

	if (err && g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT)) {
		g_error_free(err);
		g_object_unref(self);
		return;
	} else if (err) {
		g_warning("%s\n", err->message);
		g_error_free(err);
		g_object_unref(self);
		return;
	}

	GVariant *iconname_v;
	const char *iconname = NULL;
	g_variant_get(retvariant, "(v)", &iconname_v);
	g_variant_get(iconname_v, "&s", &iconname);
	g_variant_unref(retvariant);


	if (strcmp(iconname, self->iconname) == 0) {
	// Icon didn't change
		g_variant_unref(iconname_v);
		g_object_unref(self);
		return;
	}

	GSList *elem = g_slist_find_custom(self->cachedicons, iconname, (GCompareFunc)find_cached_name);

	if (elem) {
	// Cache hit
		CachedIcon *cicon = (CachedIcon*)elem->data;
		self->iconname = cicon->iconname;
		gtk_image_set_from_paintable(GTK_IMAGE(self->image), cicon->icondata);
	} else {
	// Cache miss -> cache new icon
		CachedIcon *cicon = g_malloc0(sizeof(CachedIcon));
		self->iconname = g_strdup(iconname);
		cicon->iconname = self->iconname;
		cicon->icondata = get_paintable_from_name(self->iconname,
		                                          self->iconsize);
		gtk_image_set_from_paintable(GTK_IMAGE(self->image), cicon->icondata);
		self->cachedicons = g_slist_prepend(self->cachedicons, cicon);
	}

	g_variant_unref(iconname_v);
	g_object_unref(self);
}

static int
find_cached_pixmap(CachedIcon *cicon, GVariant *pixmap)
{
	int ret;
	if (cicon->iconpixmap && g_variant_equal(cicon->iconpixmap, pixmap)) {
		ret = 0;
	} else {
		ret = 1;
	}

	return ret;
}

static void
sn_item_proxy_new_pixmaps_handler(GObject *obj, GAsyncResult *res, void *data)
{
	SnItem *self = SN_ITEM(data);
	GDBusProxy *proxy = G_DBUS_PROXY(obj);

	GError *err = NULL;
	GVariant *retvariant = g_dbus_proxy_call_finish(proxy, res, &err);
	// (v)

	if (err && g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT)) {
		g_error_free(err);
		g_object_unref(self);
		return;
	} else if (err) {
		g_warning("%s\n", err->message);
		g_error_free(err);
		g_object_unref(self);
		return;
	}

	GVariant *newpixmaps;
	g_variant_get(retvariant, "(v)", &newpixmaps);

	GVariant *pixmap = select_icon_by_size(newpixmaps, self->iconsize);

	if (g_variant_equal(pixmap, self->iconpixmap)) {
	// Icon didn't change
		g_variant_unref(pixmap);
		g_variant_unref(newpixmaps);
		g_variant_unref(retvariant);
		g_object_unref(self);
		return;
	}

	GSList *elem = g_slist_find_custom(self->cachedicons, pixmap, (GCompareFunc)find_cached_pixmap);

	if (elem) {
	// Cache hit
		CachedIcon *cicon = (CachedIcon*)elem->data;
		self->iconpixmap = cicon->iconpixmap;
		gtk_image_set_from_paintable(GTK_IMAGE(self->image), cicon->icondata);
	} else {
	// Cache miss -> cache new icon
		CachedIcon *cicon = g_malloc0(sizeof(CachedIcon));
		self->iconpixmap = g_variant_ref(pixmap);
		cicon->iconpixmap = self->iconpixmap;
		cicon->icondata = get_paintable_from_data(self->iconpixmap,
		                                          self->iconsize);
		gtk_image_set_from_paintable(GTK_IMAGE(self->image), cicon->icondata);
		self->cachedicons = g_slist_prepend(self->cachedicons, cicon);
	}

	g_variant_unref(pixmap);
	g_variant_unref(newpixmaps);
	g_variant_unref(retvariant);
	g_object_unref(self);
}

static void
sn_item_proxy_signal_handler(GDBusProxy *proxy,
                             const char *sender,
                             const char *signal,
                             GVariant *data_v,
                             void *data)
{
	SnItem *self = SN_ITEM(data);

	if (strcmp(signal, "NewIcon") == 0) {
		if (self->iconpixmap)
			g_dbus_proxy_call(proxy,
			                  "org.freedesktop.DBus.Properties.Get",
			                  g_variant_new("(ss)", "org.kde.StatusNotifierItem", "IconPixmap"),
			                  G_DBUS_CALL_FLAGS_NONE,
			                  -1,
			                  NULL,
			                  sn_item_proxy_new_pixmaps_handler,
			                  g_object_ref(self));

		if (self->iconname)
			g_dbus_proxy_call(proxy,
			                  "org.freedesktop.DBus.Properties.Get",
			                  g_variant_new("(ss)", "org.kde.StatusNotifierItem", "IconName"),
			                  G_DBUS_CALL_FLAGS_NONE,
			                  -1,
			                  NULL,
			                  sn_item_proxy_new_iconname_handler,
			                  g_object_ref(self));
	}
}

static void
sn_item_popup(SnDbusmenu *dbusmenu, SnItem *self)
{
	g_return_if_fail(SN_IS_ITEM(self) || GTK_IS_POPOVER_MENU(self->popovermenu));

	g_object_set(self, "menuvisible", TRUE, NULL);
	gtk_popover_popup(GTK_POPOVER(self->popovermenu));
}

static void
sn_item_proxy_ready_handler(GObject *obj, GAsyncResult *res, void *data)
{
	SnItem *self = SN_ITEM(data);

	GError *err = NULL;
	GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, &err);

	if (err) {
		g_warning("Failed to construct gdbusproxy for snitem: %s\n", err->message);
		g_error_free(err);
		g_object_unref(self);
		return;
	}

	g_debug("Created gdbusproxy for snitem %s %s",
		g_dbus_proxy_get_name(proxy),
		g_dbus_proxy_get_object_path(proxy));

	g_object_set(self, "proxy", proxy, NULL);

	g_signal_connect(self->proxy, "g-signal", G_CALLBACK(sn_item_proxy_signal_handler), self);

	const char *iconthemepath;
	GVariant *iconthemepath_v = g_dbus_proxy_get_cached_property(self->proxy, "IconThemePath");
	GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
	if (iconthemepath_v) {
		g_variant_get(iconthemepath_v, "&s", &iconthemepath);
		gtk_icon_theme_add_search_path(theme, iconthemepath);
		g_variant_unref(iconthemepath_v);
	}

	char *iconname = NULL;
	GVariant *iconname_v = g_dbus_proxy_get_cached_property(proxy, "IconName");
	GVariant *iconpixmaps = g_dbus_proxy_get_cached_property(proxy, "IconPixmap");

	if (iconname_v) {
		g_variant_get(iconname_v, "s", &iconname);
		if (strcmp(iconname, "") == 0) {
			g_free(iconname);
			iconname = NULL;
		}
		g_variant_unref(iconname_v);
	}

	if (iconname) {
		self->iconname = iconname;
	} else if (iconpixmaps) {
		self->iconpixmap = select_icon_by_size(iconpixmaps, self->iconsize);
	} else {
		self->iconname = g_strdup("noicon");
	}

	if (iconpixmaps)
		g_variant_unref(iconpixmaps);

	CachedIcon *cicon = g_malloc0(sizeof(CachedIcon));

	GdkPaintable *paintable;
	if (self->iconname) {
		paintable = get_paintable_from_name(self->iconname, self->iconsize);
		cicon->iconname = self->iconname;
		cicon->icondata = paintable;
	} else {
		paintable = get_paintable_from_data(self->iconpixmap, self->iconsize);
		cicon->iconpixmap = self->iconpixmap;
		cicon->icondata = paintable;
	}

	self->cachedicons = g_slist_prepend(self->cachedicons, cicon);

	gtk_image_set_from_paintable(GTK_IMAGE(self->image), paintable);

	const char *menu_buspath = NULL;
	GVariant *menu_buspath_v = g_dbus_proxy_get_cached_property(self->proxy, "Menu");

	if (menu_buspath_v && !self->exiting) {
		g_variant_get(menu_buspath_v, "&o", &menu_buspath);
		SnDbusmenu *dbusmenu = sn_dbusmenu_new(self->busname, menu_buspath, self);
		g_object_set(self, "dbusmenu", dbusmenu, NULL);

		self->popup_sig_id = g_signal_connect(self->dbusmenu,
		                                      "abouttoshowhandled",
		                                      G_CALLBACK(sn_item_popup),
		                                      self);

		g_variant_unref(menu_buspath_v);
	}
	self->ready = TRUE;
	g_object_unref(self);
}

static void
sn_item_notify_closed(GtkPopover *popover, void *data)
{
	SnItem *self = SN_ITEM(data);
	g_object_set(self, "menuvisible", FALSE, NULL);
}


static void
sn_item_leftclick_handler(GtkGestureClick *click,
                          int n_press,
                          double x,
                          double y,
                          void *data)
{
	SnItem *self = SN_ITEM(data);

	g_dbus_proxy_call(self->proxy,
	                  "Activate",
	                  g_variant_new("(ii)", 0, 0),
	                  G_DBUS_CALL_FLAGS_NONE,
	                  -1,
	                  NULL,
	                  NULL,
	                  NULL);
}

static void
sn_item_rightclick_handler(GtkGestureClick *click,
                           int n_press,
                           double x,
                           double y,
                           void *data)
{
	SnItem *self = SN_ITEM(data);
	if (!self->ready)
		return;

	g_signal_emit(self, signals[RIGHTCLICK], 0);
}

static void
sn_item_measure(GtkWidget *widget,
                GtkOrientation orientation,
                int for_size,
                int *minimum,
                int *natural,
                int *minimum_baseline,
                int *natural_baseline)
{
	SnItem *self = SN_ITEM(widget);

	switch (orientation) {
		case GTK_ORIENTATION_HORIZONTAL:
			*natural = self->iconsize;
			*minimum = self->iconsize;
			*minimum_baseline = -1;
			*natural_baseline = -1;
			break;
		case GTK_ORIENTATION_VERTICAL:
			*natural = self->iconsize;
			*minimum = self->iconsize;
			*minimum_baseline = -1;
			*natural_baseline = -1;
			break;
	}
}

static void
sn_item_size_allocate(GtkWidget *widget,
                      int width,
                      int height,
                      int baseline)
{
	SnItem *self = SN_ITEM(widget);
	gtk_widget_size_allocate(self->image, &(GtkAllocation) {0, 0, width, height}, -1);
	gtk_popover_present(GTK_POPOVER(self->popovermenu));
}

static void
sn_item_set_property(GObject *object, uint property_id, const GValue *value, GParamSpec *pspec)
{
	SnItem *self = SN_ITEM(object);

	switch (property_id) {
		case PROP_BUSNAME:
			self->busname = g_strdup(g_value_get_string(value));
			break;
		case PROP_BUSOBJ:
			self->busobj = g_strdup(g_value_get_string(value));
			break;
		case PROP_PROXY:
			self->proxy = g_value_get_object(value);
			break;
		case PROP_ICONSIZE:
			self->iconsize = g_value_get_int(value);
			break;
		case PROP_DBUSMENU:
			self->dbusmenu = g_value_get_object(value);
			break;
		case PROP_MENUVISIBLE:
			self->menu_visible = g_value_get_boolean(value);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void
sn_item_get_property(GObject *object, uint property_id, GValue *value, GParamSpec *pspec)
{
	SnItem *self = SN_ITEM(object);

	switch (property_id) {
		case PROP_BUSNAME:
			g_value_set_string(value, self->busname);
			break;
		case PROP_BUSOBJ:
			g_value_set_string(value, self->busobj);
			break;
		case PROP_ICONSIZE:
			g_value_set_int(value, self->iconsize);
			break;
		case PROP_DBUSMENU:
			g_value_set_object(value, self->dbusmenu);
			break;
		case PROP_MENUVISIBLE:
			g_value_set_boolean(value, self->menu_visible);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void
sn_item_class_init(SnItemClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

	widget_class->measure = sn_item_measure;
	widget_class->size_allocate = sn_item_size_allocate;

	object_class->set_property = sn_item_set_property;
	object_class->get_property = sn_item_get_property;

	object_class->constructed = sn_item_constructed;
	object_class->dispose = sn_item_dispose;
	object_class->finalize = sn_item_finalize;

	obj_properties[PROP_BUSNAME] =
		g_param_spec_string("busname", NULL, NULL,
		                    NULL,
		                    G_PARAM_READWRITE |
		                    G_PARAM_CONSTRUCT_ONLY |
		                    G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_BUSOBJ] =
		g_param_spec_string("busobj", NULL, NULL,
		                    NULL,
		                    G_PARAM_WRITABLE |
		                    G_PARAM_CONSTRUCT_ONLY |
		                    G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_ICONSIZE] =
		g_param_spec_int("iconsize", NULL, NULL,
		                 G_MININT,
		                 G_MAXINT,
		                 22,
		                 G_PARAM_WRITABLE |
		                 G_PARAM_CONSTRUCT_ONLY |
		                 G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_PROXY] =
		g_param_spec_object("proxy", NULL, NULL,
		                    G_TYPE_DBUS_PROXY,
		                    G_PARAM_WRITABLE |
		                    G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_DBUSMENU] =
		g_param_spec_object("dbusmenu", NULL, NULL,
		                    SN_TYPE_DBUSMENU,
		                    G_PARAM_READWRITE |
		                    G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_MENUVISIBLE] =
		g_param_spec_boolean("menuvisible", NULL, NULL,
		                     FALSE,
		                     G_PARAM_CONSTRUCT |
		                     G_PARAM_READWRITE |
		                     G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, N_PROPERTIES, obj_properties);

	signals[RIGHTCLICK] = g_signal_new("rightclick",
	                                   SN_TYPE_ITEM,
	                                   G_SIGNAL_RUN_LAST,
	                                   0,
	                                   NULL,
	                                   NULL,
	                                   NULL,
	                                   G_TYPE_NONE,
	                                   0);
}

static void
sn_item_init(SnItem *self)
{
	GtkWidget *widget = GTK_WIDGET(self);

	self->exiting = FALSE;

	gtk_widget_set_hexpand(widget, TRUE);
	gtk_widget_set_vexpand(widget, TRUE);

	self->image = gtk_image_new();
	gtk_widget_set_hexpand(self->image, TRUE);
	gtk_widget_set_vexpand(self->image, TRUE);

	gtk_widget_set_parent(self->image, widget);

	self->init_menu = g_menu_new();
	self->popovermenu = gtk_popover_menu_new_from_model(G_MENU_MODEL(self->init_menu));
	gtk_popover_menu_set_flags(GTK_POPOVER_MENU(self->popovermenu), GTK_POPOVER_MENU_NESTED);
	gtk_popover_set_has_arrow(GTK_POPOVER(self->popovermenu), FALSE);
	gtk_widget_set_parent(self->popovermenu, widget);

	GtkGesture *leftclick = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(leftclick), 1);
	g_signal_connect(leftclick, "pressed", G_CALLBACK(sn_item_leftclick_handler), self);
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(leftclick));

	GtkGesture *rightclick = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rightclick), 3);
	g_signal_connect(rightclick, "pressed", G_CALLBACK(sn_item_rightclick_handler), self);
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(rightclick));

	g_signal_connect(self->popovermenu, "closed", G_CALLBACK(sn_item_notify_closed), self);
}

static void
sn_item_constructed(GObject *obj)
{
	SnItem *self = SN_ITEM(obj);

	GDBusNodeInfo *nodeinfo = g_dbus_node_info_new_for_xml(STATUSNOTIFIERITEM_XML, NULL);
	g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION,
	                         G_DBUS_PROXY_FLAGS_NONE,
	                         nodeinfo->interfaces[0],
	                         self->busname,
	                         self->busobj,
	                         "org.kde.StatusNotifierItem",
	                         NULL,
	                         sn_item_proxy_ready_handler,
	                         g_object_ref(self));
	g_dbus_node_info_unref(nodeinfo);

	G_OBJECT_CLASS(sn_item_parent_class)->constructed(obj);
}


static void
sn_item_dispose(GObject *obj)
{
	SnItem *self = SN_ITEM(obj);
	g_debug("Disposing snitem %s %s",
	        self->busname, self->busobj);

	self->exiting = TRUE;

	if (self->dbusmenu) {
		// Unref will be called from sndbusmenu dispose function
		g_object_ref(self);

		g_signal_handler_disconnect(self->dbusmenu, self->popup_sig_id);
		self->popup_sig_id = 0;
		g_object_unref(self->dbusmenu);
		self->dbusmenu = NULL;
	}

	if (self->proxy) {
		g_object_unref(self->proxy);
		self->proxy = NULL;
	}

	if (self->popovermenu) {
		gtk_widget_unparent(self->popovermenu);
		self->popovermenu = NULL;
		g_object_unref(self->init_menu);
		self->init_menu = NULL;
	}

	if (self->image) {
		gtk_widget_unparent(self->image);
		self->image = NULL;
	}

	G_OBJECT_CLASS(sn_item_parent_class)->dispose(obj);
}

static void
sn_item_finalize(GObject *object)
{
	SnItem *self = SN_ITEM(object);

	g_free(self->busname);
	g_free(self->busobj);

	g_slist_free_full(self->cachedicons, cachedicons_free);

	G_OBJECT_CLASS(sn_item_parent_class)->finalize(object);
}

/* PUBLIC METHODS */
void
sn_item_set_menu_model(SnItem *self, GMenu* menu)
{
	g_return_if_fail(SN_IS_ITEM(self));
	g_return_if_fail(G_IS_MENU(menu));

	if (!self->popovermenu)
		return;

	gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(self->popovermenu), G_MENU_MODEL(menu));
}

void
sn_item_clear_menu_model(SnItem *self)
{
	g_return_if_fail(SN_IS_ITEM(self));

	if (!self->popovermenu)
		return;

	gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(self->popovermenu), G_MENU_MODEL(self->init_menu));
}

void
sn_item_set_actiongroup(SnItem *self, const char *prefix, GSimpleActionGroup *group)
{
	g_return_if_fail(SN_IS_ITEM(self));
	g_return_if_fail(G_IS_SIMPLE_ACTION_GROUP(group));

	gtk_widget_insert_action_group(GTK_WIDGET(self),
	                               prefix,
	                               G_ACTION_GROUP(group));
}

void
sn_item_clear_actiongroup(SnItem *self, const char *prefix)
{
	g_return_if_fail(SN_IS_ITEM(self));

	gtk_widget_insert_action_group(GTK_WIDGET(self),
	                               prefix,
	                               NULL);
}

char*
sn_item_get_busname(SnItem *self)
{
	g_return_val_if_fail(SN_IS_ITEM(self), NULL);

	char *busname;
	g_object_get(self, "busname", &busname, NULL);

	return busname;
}

gboolean
sn_item_get_popover_visible(SnItem *self)
{
	g_return_val_if_fail(SN_IS_ITEM(self), FALSE);

	gboolean visible;

	g_object_get(self, "menuvisible", &visible, NULL);

	return visible;
}

SnItem*
sn_item_new(const char *busname, const char *busobj, int iconsize)
{
	return g_object_new(SN_TYPE_ITEM,
	                    "busname", busname,
	                    "busobj", busobj,
	                    "iconsize", iconsize,
	                    NULL);
}
/* PUBLIC METHODS */
