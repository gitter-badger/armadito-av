#include <libuhuru-config.h>

#include <libuhuru/core.h>
#include "ipc.h"
#include <os/io.h>

#include "client.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <glib.h>
#include <string.h>

struct client {
  struct uhuru *uhuru;
  int sock;
  struct ipc_manager *manager;
  GMutex lock;
};

static void ipc_ping_handler(struct ipc_manager *m, void *data)
{
  struct client *cl = (struct client *)data;

#ifdef DEBUG
  uhuru_log(UHURU_LOG_SERVICE, UHURU_LOG_LEVEL_DEBUG, "ipc: ping handler called");
#endif

  ipc_manager_msg_send(cl->manager, IPC_MSG_ID_PONG, IPC_NONE_T);
}

static void scan_callback(struct uhuru_report *report, void *callback_data)
{
  struct client *cl = (struct client *)callback_data;

  g_mutex_lock(&cl->lock);
  ipc_manager_msg_send(cl->manager, 
		       IPC_MSG_ID_SCAN_FILE, 
		       IPC_STRING_T, report->path, 
		       IPC_STRING_T, uhuru_file_status_str(report->status),
		       IPC_STRING_T, report->mod_name,
		       IPC_STRING_T, report->mod_report,
		       IPC_STRING_T, uhuru_action_pretty_str(report->action),
		       IPC_INT32_T, report->progress,
		       IPC_NONE_T);
  g_mutex_unlock(&cl->lock);
}

static void ipc_scan_handler(struct ipc_manager *m, void *data)
{
  struct client *cl = (struct client *)data;
  char *path;
  int threaded;
  int recurse;
  enum uhuru_scan_flags flags = 0;
  struct uhuru_on_demand *on_demand;

#ifdef DEBUG
  uhuru_log(UHURU_LOG_SERVICE, UHURU_LOG_LEVEL_DEBUG, "ipc: scan handler called");
#endif

  ipc_manager_get_arg_at(m, 0, IPC_STRING_T, &path);
  ipc_manager_get_arg_at(m, 1, IPC_INT32_T, &threaded);
  ipc_manager_get_arg_at(m, 2, IPC_INT32_T, &recurse);

  if (threaded)
    flags |= UHURU_SCAN_THREADED;

  if (recurse)
    flags |= UHURU_SCAN_RECURSE;

  on_demand = uhuru_on_demand_new(cl->uhuru, 0, path, flags);

  uhuru_scan_add_callback(uhuru_on_demand_get_scan(on_demand), scan_callback, cl);

  uhuru_on_demand_run(on_demand);

  uhuru_on_demand_free(on_demand);

  ipc_manager_msg_send(cl->manager, IPC_MSG_ID_SCAN_END, IPC_NONE_T);

  if (os_close(cl->sock) < 0) {
    uhuru_log(UHURU_LOG_SERVICE, UHURU_LOG_LEVEL_WARNING, "closing socket %d failed (%d)", cl->sock, errno);
  }
  cl->sock = -1;

#ifdef DEBUG
  uhuru_log(UHURU_LOG_SERVICE, UHURU_LOG_LEVEL_DEBUG, "ipc: scan handler finished");
#endif
}

static void info_send(struct ipc_manager *manager, struct uhuru_info *info)
{
  struct uhuru_module_info **m;

  for(m = info->module_infos; *m != NULL; m++) {
    ipc_manager_msg_begin(manager, IPC_MSG_ID_INFO_MODULE);
    ipc_manager_msg_add(manager, 
			IPC_STRING_T, (*m)->name, 
			IPC_STRING_T, uhuru_update_status_str((*m)->mod_status), 
			IPC_STRING_T, (*m)->update_date, 
			IPC_NONE_T);

    if ((*m)->base_infos != NULL) {
      struct uhuru_base_info **b;

      for(b = (*m)->base_infos; *b != NULL; b++)
	ipc_manager_msg_add(manager, 
			    IPC_STRING_T, (*b)->name,
			    IPC_STRING_T, (*b)->date,
			    IPC_STRING_T, (*b)->version,
			    IPC_INT32_T, (*b)->signature_count,
			    IPC_STRING_T, (*b)->full_path,
			    IPC_NONE_T);
    }

    ipc_manager_msg_end(manager);
  }

  ipc_manager_msg_send(manager, 
		       IPC_MSG_ID_INFO_END, 
		       IPC_STRING_T, uhuru_update_status_str(info->global_status), 
		       IPC_NONE_T);
}

static void ipc_info_handler(struct ipc_manager *manager, void *data)
{
  struct client *cl = (struct client *)data;
  struct uhuru_info *info;

#ifdef DEBUG
  uhuru_log(UHURU_LOG_SERVICE, UHURU_LOG_LEVEL_DEBUG, "ipc: info handler called");
#endif

  info = uhuru_info_new(cl->uhuru);

  info_send(manager, info);

  uhuru_info_free(info);

  if (os_close(cl->sock) < 0) {
    uhuru_log(UHURU_LOG_SERVICE, UHURU_LOG_LEVEL_WARNING, "closing socket %d failed (%d)", cl->sock, errno);
  }

  cl->sock = -1;

#ifdef DEBUG
  uhuru_log(UHURU_LOG_SERVICE, UHURU_LOG_LEVEL_DEBUG, "ipc: info handler finished");
#endif
}

struct client *client_new(int client_sock, struct uhuru *uhuru)
{
  struct client *cl = (struct client *)malloc(sizeof(struct client));

  cl->uhuru = uhuru;
  cl->sock = client_sock;
  cl->manager = ipc_manager_new(cl->sock);

  ipc_manager_add_handler(cl->manager, IPC_MSG_ID_PING, ipc_ping_handler, cl);
  ipc_manager_add_handler(cl->manager, IPC_MSG_ID_SCAN, ipc_scan_handler, cl);
  ipc_manager_add_handler(cl->manager, IPC_MSG_ID_INFO, ipc_info_handler, cl);

  g_mutex_init(&cl->lock);

  return cl;
}

void client_free(struct client *cl)
{
  ipc_manager_free(cl->manager);

  g_mutex_clear(&cl->lock);

  free(cl);
}

int client_process(struct client *cl)
{
  int ret;

  if (cl->sock < 0)
    return 0;

  ret = ipc_manager_receive(cl->manager);

  return ret;
}
