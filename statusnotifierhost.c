#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

#include "dwlbtray.h"


static void handle_method_call(GDBusConnection* conn, const char* sender,
		const char* object_path, const char* iface, const char* method,
		GVariant* parameters, GDBusMethodInvocation* invocation,
		StatusNotifierHost* snhost
);


static GVariant* handle_get_prop(GDBusConnection* conn, const char* sender,
		const char* obj_path, const char* iface_name,
		const char* prop, GError** error, StatusNotifierHost* snhost
);


static GDBusInterfaceVTable interface_vtable = {
	(GDBusInterfaceMethodCallFunc)handle_method_call,
	(GDBusInterfaceGetPropertyFunc)handle_get_prop,
	NULL
};


void
dwlb_request_resize(StatusNotifierHost *snhost)
{
	if (snhost->noitems <= 1)
		snhost->cursize = 22;
	else
		snhost->cursize = 22 * snhost->noitems - 6; // dunno why substract 6 to make it align, just trial and error until it worked

	struct sockaddr_un sockaddr;
	sockaddr.sun_family = AF_UNIX;
	char *socketpath = g_strdup_printf("%s/dwlb/dwlb-0", g_get_user_runtime_dir());
	snprintf(sockaddr.sun_path, sizeof(sockaddr.sun_path), "%s", socketpath);
	char *sockbuf = NULL;
	if (snhost->traymon) {
		sockbuf = g_strdup_printf("%s %s %i", snhost->traymon, "resize", snhost->cursize);
	}
	else {
		sockbuf = g_strdup_printf("%s %s %i", "selected", "resize", snhost->cursize);
	}

	size_t len = strlen(sockbuf);
	int sock_fd = socket(AF_UNIX, SOCK_STREAM, 1);

	int connstatus = connect(sock_fd, (struct sockaddr *)&sockaddr, sizeof(sockaddr));
	if (connstatus != 0) {
		g_error("Error connecting to dwlb socket");
	}

	if (send(sock_fd, sockbuf, len, 0) == -1)
		fprintf(stderr, "Could not send size update to %s\n", sockaddr.sun_path);
	close(sock_fd);

	g_free(sockbuf);
	g_free(socketpath);
}

GDBusNodeInfo*
get_interface_info(const char *xmlpath)
{
	char *contents;
	GError *err = NULL;

	g_file_get_contents(xmlpath, &contents, NULL, &err);

	if (err) {
		fprintf(stderr, "%s\n", err->message);
		g_error_free(err);
		return NULL;
	}

	GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(contents, &err);
	g_free(contents);

	if (err) {
		fprintf(stderr, "%s\n", err->message);
		g_error_free(err);
		return NULL;
	}

	return node;
}



static void
register_statusnotifieritem(const char *busname,
                            const char *busobj,
                            StatusNotifierHost *snhost)
{
	StatusNotifierItem *snitem;
	snitem = g_malloc(sizeof(StatusNotifierItem));

	snitem->busname = g_strdup(busname);
	snitem->busobj = g_strdup(busobj);

	snitem->host = snhost;

	char *xml_path = g_strdup_printf("%s%s", RESOURCE_PATH, "/StatusNotifierItem.xml");
	snitem->nodeinfo = get_interface_info(xml_path);
	g_free(xml_path);

	snitem->action_cb_data_slist = NULL;
	snitem->actiongroup = NULL;
	snitem->icon = NULL;
	snitem->iconname = NULL;
	snitem->iconpixmap_v = NULL;
	snitem->menu = NULL;
	snitem->menunodeinfo = NULL;
	snitem->menuobj = NULL;
	snitem->menuproxy = NULL;
	snitem->paintable = NULL;
	snitem->popovermenu = NULL;

	snhost->noitems = snhost->noitems + 1;

	dwlb_request_resize(snhost);

	snhost->trayitems = g_slist_prepend(snhost->trayitems, snitem);

	g_dbus_proxy_new(snhost->conn,
                         G_DBUS_PROXY_FLAGS_NONE,
                         snitem->nodeinfo->interfaces[0],
                         snitem->busname,
                         snitem->busobj,
                         "org.kde.StatusNotifierItem",
                         NULL,
                         (GAsyncReadyCallback)create_trayitem,
                         snitem);

	GError *err = NULL;
	g_dbus_connection_emit_signal(snhost->conn,
	                              NULL,
	                              "/StatusNotifierWatcher",
	                              "org.kde.StatusNotifierWatcher",
	                              "StatusNotifierItemRegistered",
	                              g_variant_new("(s)", snitem->busname),
	                              &err);
	if (err) {
		g_debug("%s\n", err->message);
		g_error_free(err);
	}
}


static int
find_snitem(StatusNotifierItem *snitem, char *busname_match)
{
	if (strcmp(snitem->busname, busname_match) == 0)
		return 0;
	else
		return -1;
}


static void
unregister_statusnotifieritem(StatusNotifierItem *snitem, StatusNotifierHost *snhost)
{
	g_debug("item %s is closing\n", snitem->busname);
	if (snitem->popovermenu)
		gtk_widget_unparent(snitem->popovermenu);

	if (snitem->icon)
		gtk_box_remove(GTK_BOX(snhost->box), snitem->icon);

	GError *err = NULL;
	g_dbus_connection_emit_signal(snhost->conn,
                                      NULL,
	                              "/StatusNotifierWatcher",
	                              "org.kde.StatusNotifierWatcher",
	                              "StatusNotifierItemUnregistered",
	                              g_variant_new("(s)", snitem->busname),
	                              &err);
	if (err) {
		g_debug("%s\n", err->message);
		g_error_free(err);
	}

	if (snitem->menu) {
		g_object_unref(snitem->menuproxy);
		g_slist_free_full(snitem->action_cb_data_slist, g_free);
		g_object_unref(snitem->menu);
	}

	g_object_unref(snitem->paintable);
	if (snitem->iconpixmap_v)
		g_variant_unref(snitem->iconpixmap_v);

	g_object_unref(snitem->proxy);
	g_object_unref(snitem->actiongroup);
	g_dbus_node_info_unref(snitem->menunodeinfo);
	g_dbus_node_info_unref(snitem->nodeinfo);
	g_free(snitem->busname);
	g_free(snitem->busobj);
	g_free(snitem->menuobj);
	snhost->trayitems = g_slist_remove(snhost->trayitems, snitem);
	g_free(snitem);
	snitem = NULL;

	snhost->noitems = snhost->noitems - 1;
	dwlb_request_resize(snhost);
}


/*
 * static void
 * unregister_all(StatusNotifierHost *snhost)
 * {
 * 	g_slist_foreach(snhost->trayitems, (GFunc)unregister_statusnotifieritem, snhost);
 * }
*/


static void
handle_method_call(GDBusConnection *conn,
                   const char *sender,
                   const char *object_path,
                   const char *interface_name,
                   const char *method_name,
                   GVariant *parameters,
                   GDBusMethodInvocation *invocation,
                   StatusNotifierHost *snhost)
{
	if (strcmp(method_name, "RegisterStatusNotifierItem") == 0) {
		char *param;
		const char *busobj;

		g_variant_get(parameters, "(s)", &param);

		if (g_str_has_prefix(param, "/")) {
			busobj = param;
		} else {
			busobj = "/StatusNotifierItem";
		}

		register_statusnotifieritem(sender, busobj, snhost);
		g_dbus_method_invocation_return_value(invocation, NULL);
		g_free(param);
	} else {
		g_dbus_method_invocation_return_dbus_error(invocation,
                                                           "org.freedesktop.DBus.Error.UnknownMethod",
		                                           "Unknown method");
	}
}


static void
add_trayitem_name_to_builder(StatusNotifierItem *snitem, GVariantBuilder *builder)
{
	g_variant_builder_add_value(builder, g_variant_new_string(snitem->busname));
}

static GVariant*
handle_get_prop(GDBusConnection* conn,
                const char* sender,
                const char* object_path,
                const char* interface_name,
                const char* property_name,
                GError** err,
                StatusNotifierHost *snhost)
{
	if (strcmp(property_name, "ProtocolVersion") == 0) {
		return g_variant_new("i", 0);
	} else if (strcmp(property_name, "IsStatusNotifierHostRegistered") == 0) {
		return g_variant_new("b", TRUE);
	} else if (strcmp(property_name, "RegisteredStatusNotifierItems") == 0) {
		if (!snhost->trayitems)
			return g_variant_new("as", NULL);

		GVariantBuilder *builder = g_variant_builder_new(G_VARIANT_TYPE_ARRAY);
		g_slist_foreach(snhost->trayitems, (GFunc)add_trayitem_name_to_builder, builder);
		GVariant *as = g_variant_builder_end(builder);

		g_variant_builder_unref(builder);

		return as;
	} else {
		g_set_error(err,
                            G_DBUS_ERROR,
		            G_DBUS_ERROR_UNKNOWN_PROPERTY,
		            "Unknown property '%s'.",
		            property_name);
		g_error_free(*err);
		return NULL;
	}
}

/*
 * void
 * terminate_statusnotifierhost(StatusNotifierHost *snhost)
 * {
 * 	g_dbus_connection_signal_unsubscribe(snhost->conn, snhost->nameowner_sig_sub_id);
 * 	g_bus_unown_name(snhost->owner_id);
 * 	unregister_all(snhost);
 * 	g_slist_free(snhost->trayitems);
 * 	g_dbus_node_info_unref(snhost->nodeinfo);
 * 	g_free(snhost);
 * }
 */

// Finds trayitems which dropped from the bus and untracks them
static void
monitor_bus(GDBusConnection* conn,
            const char* sender,
            const char* objpath,
            const char* iface_name,
            const char* signame,
            GVariant *params,
            StatusNotifierHost *snhost)
{
	if (strcmp(signame, "NameOwnerChanged") == 0) {
		if (!snhost->trayitems)
			return;

		char *name;
		char *old_owner;
		char *new_owner;

		g_variant_get(params, "(sss)", &name, &old_owner, &new_owner);

		if (strcmp(new_owner, "") == 0) {
			GSList *pmatch = g_slist_find_custom(snhost->trayitems, name, (GCompareFunc)find_snitem);


			if (!pmatch) {
				g_free(name);
				g_free(old_owner);
				g_free(new_owner);
				return;
			}

			StatusNotifierItem *snitem = pmatch->data;
			unregister_statusnotifieritem(snitem, snhost);
		}
		g_free(name);
		g_free(old_owner);
		g_free(new_owner);
	}
}


static void
bus_acquired_handler(GDBusConnection *conn, const char *busname, StatusNotifierHost *snhost)
{
	snhost->conn = conn;
	GError *err = NULL;
	snhost->bus_obj_reg_id = g_dbus_connection_register_object(conn,
                                                                   "/StatusNotifierWatcher",
                                                                   snhost->nodeinfo->interfaces[0],
                                                                   &interface_vtable,
                                                                   snhost,  // udata
                                                                   NULL,  // udata_free_func
                                                                   &err);

	if (err) {
		g_error("%s\n", err->message);
		g_error_free(err);
		exit(-1);
	}

	snhost->nameowner_sig_sub_id = g_dbus_connection_signal_subscribe(conn,
                                                                          NULL,  // Listen to all senders);
                                                                          "org.freedesktop.DBus",
	                                                                  "NameOwnerChanged",
	                                                                  NULL,  // Match all obj paths
	                                                                  NULL,  // Match all arg0s
	                                                                  G_DBUS_SIGNAL_FLAGS_NONE,
	                                                                  (GDBusSignalCallback)monitor_bus,
	                                                                  snhost,  // udata
	                                                                  NULL);  // udata free func
}


static void
name_acquired_handler(GDBusConnection *conn, const char *busname, StatusNotifierHost *snhost)
{
	GError *err = NULL;

	g_dbus_connection_emit_signal(conn,
                                      NULL,
                                      "/StatusNotifierWatcher",
                                      "org.kde.StatusNotifierWatcher",
                                      "StatusNotifierHostRegistered",
                                      NULL,
                                      &err);

	if (err) {
		g_debug("%s\n", err->message);
		g_error_free(err);
	}
}


static void
name_lost_handler(GDBusConnection *conn, const char *busname, StatusNotifierHost *snhost)
{
	g_error("Could not acquire %s\n", busname);
	exit(-1);
}


StatusNotifierHost*
start_statusnotifierhost()
{
	StatusNotifierHost *snhost = g_malloc0(sizeof(StatusNotifierHost));
	char *xml_path = g_strdup_printf("%s%s", RESOURCE_PATH, "/StatusNotifierWatcher.xml");
	snhost->nodeinfo = get_interface_info(xml_path);
	g_free(xml_path);

	snhost->height = 22;
	snhost->margin = 4;
	snhost->noitems = 0;

	snhost->owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
                                          "org.kde.StatusNotifierWatcher",
                                          G_BUS_NAME_OWNER_FLAGS_NONE,
                                          (GBusAcquiredCallback)bus_acquired_handler,
                                          (GBusNameAcquiredCallback)name_acquired_handler,
                                          (GBusNameLostCallback)name_lost_handler,
                                          snhost,
                                          NULL); // (GDestroyNotify)snhost_finalize);

	return snhost;
}