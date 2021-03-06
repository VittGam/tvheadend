/*
 *  Services
 *  Copyright (C) 2010 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "tvheadend.h"
#include "service.h"
#include "subscriptions.h"
#include "streaming.h"
#include "packet.h"
#include "channels.h"
#include "notify.h"
#include "service_mapper.h"
#include "atomic.h"
#include "htsp_server.h"
#include "lang_codes.h"
#include "descrambler.h"
#include "input.h"

static void service_data_timeout(void *aux);
static void service_class_save(struct idnode *self);

struct service_queue service_all;

static const void *
service_class_channel_get ( void *obj )
{
  service_t *svc = obj;
  channel_service_mapping_t *csm;

  htsmsg_t *l = htsmsg_create_list();
  LIST_FOREACH(csm, &svc->s_channels, csm_svc_link)
    htsmsg_add_str(l, NULL, idnode_uuid_as_str(&csm->csm_chn->ch_id));
  
  return l;
}

static char *
service_class_channel_rend ( void *obj )
{
  char *str;
  service_t *svc = obj;
  channel_service_mapping_t *csm;

  htsmsg_t *l = htsmsg_create_list();
  LIST_FOREACH(csm, &svc->s_channels, csm_svc_link)
    htsmsg_add_str(l, NULL, idnode_get_title(&csm->csm_chn->ch_id));

  str = htsmsg_list_2_csv(l);
  htsmsg_destroy(l);
  return str;
}

static int
service_class_channel_set
  ( void *obj, const void *p )
{
  service_t *svc = obj;
  htsmsg_t  *chns = (htsmsg_t*)p;
  const char *str;
  htsmsg_field_t *f;
  channel_t *ch;
  channel_service_mapping_t *csm;

  /* Mark all for deletion */
  LIST_FOREACH(csm, &svc->s_channels, csm_svc_link)
    csm->csm_mark = 1;

  /* Make new links */
  HTSMSG_FOREACH(f, chns) {
    if ((str = htsmsg_field_get_str(f)))
      if ((ch = channel_find(str)))
        service_mapper_link(svc, ch, svc);
  }

  /* Delete unlinked */
  service_mapper_clean(svc, NULL, svc);

  /* no save - the link information is in the saved channel record */
  /* only send a notify about the change to other clients */
  idnode_notify_simple(&svc->s_id);
  return 0;
}

static htsmsg_t *
service_class_channel_enum
  ( void *obj )
{
  htsmsg_t *p, *m = htsmsg_create_map();
  htsmsg_add_str(m, "type",  "api");
  htsmsg_add_str(m, "uri",   "channel/list");
  htsmsg_add_str(m, "event", "channel");
  p = htsmsg_create_map();
  htsmsg_add_u32(p, "enum", 1);
  htsmsg_add_msg(m, "params", p);
  return m;
}

static const char *
service_class_get_title ( idnode_t *self )
{
  return service_get_channel_name((service_t*)self);
}

static const void *
service_class_encrypted_get ( void *p )
{
  static int t;
  service_t *s = p;
  pthread_mutex_lock(&s->s_stream_mutex);
  t = service_is_encrypted(s);
  pthread_mutex_unlock(&s->s_stream_mutex);
  return &t;
}

const idclass_t service_class = {
  .ic_class      = "service",
  .ic_caption    = "Service",
  .ic_save       = service_class_save,
  .ic_get_title  = service_class_get_title,
  .ic_properties = (const property_t[]){
    {
      .type     = PT_BOOL,
      .id       = "enabled",
      .name     = "Enabled",
      .off      = offsetof(service_t, s_enabled),
    },
    {
      .type     = PT_STR,
      .islist   = 1,
      .id       = "channel",
      .name     = "Channel",
      .get      = service_class_channel_get,
      .set      = service_class_channel_set,
      .list     = service_class_channel_enum,
      .rend     = service_class_channel_rend,
      .opts     = PO_NOSAVE
    },
    {
      .type     = PT_BOOL,
      .id       = "encrypted",
      .name     = "Encrypted",
      .get      = service_class_encrypted_get,
      .opts     = PO_NOSAVE | PO_RDONLY
    },
    {}
  }
};

/**
 *
 */
static void
stream_init(elementary_stream_t *st)
{
  st->es_cc = -1;

  st->es_startcond = 0xffffffff;
  st->es_curdts = PTS_UNSET;
  st->es_curpts = PTS_UNSET;
  st->es_prevdts = PTS_UNSET;

  st->es_pcr_real_last = PTS_UNSET;
  st->es_pcr_last      = PTS_UNSET;
  st->es_pcr_drift     = 0;
  st->es_pcr_recovery_fails = 0;

  st->es_blank = 0;
}


/**
 *
 */
static void
stream_clean(elementary_stream_t *st)
{
  free(st->es_priv);
  st->es_priv = NULL;

  /* Clear reassembly buffers */

  st->es_startcode = 0;
  
  sbuf_free(&st->es_buf);
  sbuf_free(&st->es_buf_ps);
  sbuf_free(&st->es_buf_a);

  if(st->es_curpkt != NULL) {
    pkt_ref_dec(st->es_curpkt);
    st->es_curpkt = NULL;
  }

  free(st->es_global_data);
  st->es_global_data = NULL;
  st->es_global_data_len = 0;

  free(st->es_section);
  st->es_section = NULL;
}

/**
 *
 */
void
service_stream_destroy(service_t *t, elementary_stream_t *es)
{
  caid_t *c;

  if(t->s_status == SERVICE_RUNNING)
    stream_clean(es);

  avgstat_flush(&es->es_rate);
  avgstat_flush(&es->es_cc_errors);

  if (t->s_last_es == es) {
    t->s_last_pid = -1;
    t->s_last_es = NULL;
  }

  TAILQ_REMOVE(&t->s_components, es, es_link);

  while ((c = LIST_FIRST(&es->es_caids)) != NULL) {
    LIST_REMOVE(c, link);
    free(c);
  }

  free(es->es_section);
  free(es->es_nicename);
  free(es);
}

/**
 * Service lock must be held
 */
static void
service_stop(service_t *t)
{
  th_descrambler_t *td;
  elementary_stream_t *st;
 
  gtimer_disarm(&t->s_receive_timer);

  t->s_stop_feed(t);

  pthread_mutex_lock(&t->s_stream_mutex);

  while((td = LIST_FIRST(&t->s_descramblers)) != NULL)
    td->td_stop(td);

  t->s_tt_commercial_advice = COMMERCIAL_UNKNOWN;
 
  assert(LIST_FIRST(&t->s_streaming_pad.sp_targets) == NULL);
  assert(LIST_FIRST(&t->s_subscriptions) == NULL);

  /**
   * Clean up each stream
   */
  TAILQ_FOREACH(st, &t->s_components, es_link)
    stream_clean(st);

  t->s_status = SERVICE_IDLE;

  pthread_mutex_unlock(&t->s_stream_mutex);
}


/**
 * Remove the given subscriber from the service
 *
 * if s == NULL all subscribers will be removed
 *
 * Global lock must be held
 */
void
service_remove_subscriber(service_t *t, th_subscription_t *s,
                          int reason)
{
  lock_assert(&global_lock);

  if(s == NULL) {
    while((s = LIST_FIRST(&t->s_subscriptions)) != NULL) {
      subscription_unlink_service(s, reason);
    }
  } else {
    subscription_unlink_service(s, reason);
  }

  if(LIST_FIRST(&t->s_subscriptions) == NULL)
    service_stop(t);
}


/**
 *
 */
int
service_start(service_t *t, int instance)
{
  elementary_stream_t *st;
  int r, timeout = 10;

  lock_assert(&global_lock);

  tvhtrace("service", "starting %s", t->s_nicename);

  assert(t->s_status != SERVICE_RUNNING);
  t->s_streaming_status = 0;
  t->s_scrambled_seen   = 0;

  if((r = t->s_start_feed(t, instance)))
    return r;

  descrambler_service_start(t);

  pthread_mutex_lock(&t->s_stream_mutex);

  t->s_status = SERVICE_RUNNING;
  t->s_current_pts = PTS_UNSET;

  /**
   * Initialize stream
   */
  TAILQ_FOREACH(st, &t->s_components, es_link)
    stream_init(st);

  pthread_mutex_unlock(&t->s_stream_mutex);

  if(t->s_grace_period != NULL)
    timeout = t->s_grace_period(t);

  gtimer_arm(&t->s_receive_timer, service_data_timeout, t, timeout);
  return 0;
}


/**
 * Main entry point for starting a service based on a channel
 */
service_instance_t *
service_find_instance
  (service_t *s, channel_t *ch, service_instance_list_t *sil,
   int *error, int weight)
{
  channel_service_mapping_t *csm;
  service_instance_t *si, *next;

  lock_assert(&global_lock);

  /* Build list */
  TAILQ_FOREACH(si, sil, si_link)
    si->si_mark = 1;

  if (ch) {
    LIST_FOREACH(csm, &ch->ch_services, csm_chn_link) {
      s = csm->csm_svc;
      if (s->s_is_enabled(s))
        s->s_enlist(s, sil);
    }
  } else {
    s->s_enlist(s, sil);
  }

  /* Clean */
  for(si = TAILQ_FIRST(sil); si != NULL; si = next) {
    next = TAILQ_NEXT(si, si_link);
    if(si->si_mark)
      service_instance_destroy(sil, si);
  }
  
  /* Debug */
  TAILQ_FOREACH(si, sil, si_link) {
    const char *name = ch ? channel_get_name(ch) : NULL;
    if (!name && s) name = s->s_nicename;
    tvhdebug("service", "%s si %p weight %d prio %d error %d",
             name, si, si->si_weight, si->si_prio, si->si_error);
  }

  /* Already running? */
  TAILQ_FOREACH(si, sil, si_link)
    if(si->si_s->s_status == SERVICE_RUNNING && si->si_error == 0) {
      tvhtrace("service", "return already running %p", si);
      return si;
    }

  /* Forced or Idle */
  TAILQ_FOREACH(si, sil, si_link)
    if(si->si_weight <= 0 && si->si_error == 0)
      break;

  /* Bump someone */
  if (!si) {
    TAILQ_FOREACH_REVERSE(si, sil, service_instance_list, si_link)
      if (weight > si->si_weight && si->si_error == 0)
        break;
  }

  /* Failed */
  if(si == NULL) {
    if (*error < SM_CODE_NO_FREE_ADAPTER)
      *error = SM_CODE_NO_FREE_ADAPTER;
    return NULL;
  }

  /* Start */
  tvhtrace("service", "will start new instance %d", si->si_instance);
  if (service_start(si->si_s, si->si_instance)) {
    tvhtrace("service", "tuning failed");
    si->si_error = SM_CODE_TUNING_FAILED;
    if (*error < SM_CODE_TUNING_FAILED)
      *error = SM_CODE_TUNING_FAILED;
    si = NULL;
  }
  return si;
}


/**
 *
 */
void
service_unref(service_t *t)
{
  if((atomic_add(&t->s_refcount, -1)) == 1) {
    free(t->s_nicename);
    free(t);
  }
}


/**
 *
 */
void
service_ref(service_t *t)
{
  atomic_add(&t->s_refcount, 1);
}



/**
 * Destroy a service
 */
void
service_destroy(service_t *t, int delconf)
{
  elementary_stream_t *st;
  th_subscription_t *s;
  channel_service_mapping_t *csm;

  if(t->s_delete != NULL)
    t->s_delete(t, delconf);

  lock_assert(&global_lock);
  
  service_mapper_remove(t);

  while((s = LIST_FIRST(&t->s_subscriptions)) != NULL) {
    subscription_unlink_service(s, SM_CODE_SOURCE_DELETED);
  }

  while ((csm = LIST_FIRST(&t->s_channels))) {
    LIST_REMOVE(csm, csm_svc_link);
    LIST_REMOVE(csm, csm_chn_link);
    free(csm);
  }

  idnode_unlink(&t->s_id);

  if(t->s_status != SERVICE_IDLE)
    service_stop(t);

  t->s_status = SERVICE_ZOMBIE;

  while((st = TAILQ_FIRST(&t->s_components)) != NULL)
    service_stream_destroy(t, st);

  avgstat_flush(&t->s_rate);

  TAILQ_REMOVE(&service_all, t, s_all_link);

  service_unref(t);
}

static int
service_channel_number ( service_t *s )
{
  return 0;
}

static const char *
service_channel_name ( service_t *s )
{
  return NULL;
}

static const char *
service_provider_name ( service_t *s )
{
  return NULL;
}

/**
 * Create and initialize a new service struct
 */
service_t *
service_create0
  ( service_t *t, const idclass_t *class, const char *uuid,
    int source_type, htsmsg_t *conf )
{
  idnode_insert(&t->s_id, uuid, class);

  lock_assert(&global_lock);
  
  TAILQ_INSERT_TAIL(&service_all, t, s_all_link);

  pthread_mutex_init(&t->s_stream_mutex, NULL);
  pthread_cond_init(&t->s_tss_cond, NULL);
  t->s_source_type = source_type;
  t->s_refcount = 1;
  t->s_enabled = 1;
  t->s_channel_number = service_channel_number;
  t->s_channel_name   = service_channel_name;
  t->s_provider_name  = service_provider_name;
  TAILQ_INIT(&t->s_components);
  t->s_last_pid = -1;

  streaming_pad_init(&t->s_streaming_pad);
  
  /* Load config */
  if (conf)
    service_load(t, conf);

  return t;
}

/**
 * Find a service based on the given identifier
 */
service_t *
service_find(const char *identifier)
{
  return idnode_find(identifier, &service_class);
}


/**
 *
 */
static void 
service_stream_make_nicename(service_t *t, elementary_stream_t *st)
{
  char buf[200];
  if(st->es_pid != -1)
    snprintf(buf, sizeof(buf), "%s: %s @ #%d", 
	     service_nicename(t),
	     streaming_component_type2txt(st->es_type), st->es_pid);
  else
    snprintf(buf, sizeof(buf), "%s: %s", 
	     service_nicename(t),
	     streaming_component_type2txt(st->es_type));

  free(st->es_nicename);
  st->es_nicename = strdup(buf);
}


/**
 *
 */
void 
service_make_nicename(service_t *t)
{
  char buf[200];
  source_info_t si;
  elementary_stream_t *st;

  lock_assert(&t->s_stream_mutex);

  t->s_setsourceinfo(t, &si);

  snprintf(buf, sizeof(buf), 
	   "%s%s%s%s%s",
	   si.si_adapter ?: "", si.si_adapter && si.si_mux     ? "/" : "",
	   si.si_mux     ?: "", si.si_mux     && si.si_service ? "/" : "",
	   si.si_service ?: "");

  service_source_info_free(&si);

  free(t->s_nicename);
  t->s_nicename = strdup(buf);

  TAILQ_FOREACH(st, &t->s_components, es_link)
    service_stream_make_nicename(t, st);
}


/**
 * Add a new stream to a service
 */
elementary_stream_t *
service_stream_create(service_t *t, int pid,
			streaming_component_type_t type)
{
  elementary_stream_t *st;
  int i = 0;
  int idx = 0;
  lock_assert(&t->s_stream_mutex);

  TAILQ_FOREACH(st, &t->s_components, es_link) {
    if(st->es_index > idx)
      idx = st->es_index;
    i++;
    if(pid != -1 && st->es_pid == pid)
      return st;
  }

  st = calloc(1, sizeof(elementary_stream_t));
  st->es_index = idx + 1;

  st->es_type = type;

  TAILQ_INSERT_TAIL(&t->s_components, st, es_link);
  st->es_service = t;

  st->es_pid = pid;

  avgstat_init(&st->es_rate, 10);
  avgstat_init(&st->es_cc_errors, 10);

  service_stream_make_nicename(t, st);

  if(t->s_flags & S_DEBUG)
    tvhlog(LOG_DEBUG, "service", "Add stream %s", st->es_nicename);

  if(t->s_status == SERVICE_RUNNING)
    stream_init(st);

  return st;
}



/**
 * Find an elementary stream in a service
 */
elementary_stream_t *
service_stream_find_(service_t *t, int pid)
{
  elementary_stream_t *st;
 
  lock_assert(&t->s_stream_mutex);

  TAILQ_FOREACH(st, &t->s_components, es_link) {
    if(st->es_pid == pid) {
      t->s_last_es = st;
      t->s_last_pid = pid;
      return st;
    }
  }
  return NULL;
}

/**
 *
 */
static void
service_data_timeout(void *aux)
{
  service_t *t = aux;

  pthread_mutex_lock(&t->s_stream_mutex);

  if(!(t->s_streaming_status & TSS_PACKETS))
    service_set_streaming_status_flags(t, TSS_GRACEPERIOD);

  pthread_mutex_unlock(&t->s_stream_mutex);
}

/**
 *
 */
int
service_is_sdtv(service_t *t)
{
  if (t->s_servicetype == ST_SDTV)
    return 1;
  else if (t->s_servicetype == ST_NONE) {
    elementary_stream_t *st;
    TAILQ_FOREACH(st, &t->s_components, es_link)
      if (SCT_ISVIDEO(st->es_type) && st->es_height < 720)
        return 1;
  }
  return 0;
}

int
service_is_hdtv(service_t *t)
{
  if (t->s_servicetype == ST_HDTV)
    return 1;
  else if (t->s_servicetype == ST_NONE) {
    elementary_stream_t *st;
    TAILQ_FOREACH(st, &t->s_components, es_link)
      if (SCT_ISVIDEO(st->es_type) && st->es_height >= 720)
        return 1;
  }
  return 0;
}

/**
 *
 */
int
service_is_radio(service_t *t)
{
  int ret = 0;
  if (t->s_servicetype == ST_RADIO)
    return 1;
  else if (t->s_servicetype == ST_NONE) {
    elementary_stream_t *st;
    TAILQ_FOREACH(st, &t->s_components, es_link) {
      if (SCT_ISVIDEO(st->es_type))
        return 0;
      else if (SCT_ISAUDIO(st->es_type))
        ret = 1;
    }
  }
  return ret;
}

/**
 * Is encrypted
 */
int
service_is_encrypted(service_t *t)
{
  elementary_stream_t *st;
  TAILQ_FOREACH(st, &t->s_components, es_link)
    if (st->es_type == SCT_CA)
      return 1;
  return 0;
}

/*
 * String describing service type
 */
const char *
service_servicetype_txt ( service_t *s )
{
  static const char *types[] = {
    "HDTV", "SDTV", "Radio", "Other"
  };
  if (service_is_hdtv(s))  return types[0];
  if (service_is_sdtv(s))  return types[1];
  if (service_is_radio(s)) return types[2];
  return types[3];
}


/**
 *
 */
void
service_set_streaming_status_flags_(service_t *t, int set)
{
  int n;
  streaming_message_t *sm;
  lock_assert(&t->s_stream_mutex);

  n = t->s_streaming_status;
  
  n |= set;

  if(n == t->s_streaming_status)
    return; // Already set

  t->s_streaming_status = n;

  tvhlog(LOG_DEBUG, "service", "%s: Status changed to %s%s%s%s%s%s%s",
	 service_nicename(t),
	 n & TSS_INPUT_HARDWARE ? "[Hardware input] " : "",
	 n & TSS_INPUT_SERVICE  ? "[Input on service] " : "",
	 n & TSS_MUX_PACKETS    ? "[Demuxed packets] " : "",
	 n & TSS_PACKETS        ? "[Reassembled packets] " : "",
	 n & TSS_NO_DESCRAMBLER ? "[No available descrambler] " : "",
	 n & TSS_NO_ACCESS      ? "[No access] " : "",
	 n & TSS_GRACEPERIOD    ? "[Graceperiod expired] " : "");

  sm = streaming_msg_create_code(SMT_SERVICE_STATUS,
				 t->s_streaming_status);
  streaming_pad_deliver(&t->s_streaming_pad, sm);
  streaming_msg_free(sm);

  pthread_cond_broadcast(&t->s_tss_cond);
}


/**
 * Restart output on a service.
 * Happens if the stream composition changes. 
 * (i.e. an AC3 stream disappears, etc)
 */
void
service_restart(service_t *t, int had_components)
{
  streaming_message_t *sm;
  pthread_mutex_lock(&t->s_stream_mutex);

  if(had_components) {
    sm = streaming_msg_create_code(SMT_STOP, SM_CODE_SOURCE_RECONFIGURED);
    streaming_pad_deliver(&t->s_streaming_pad, sm);
    streaming_msg_free(sm);
  }

  descrambler_service_start(t);

  if(TAILQ_FIRST(&t->s_components) != NULL) {
    sm = streaming_msg_create_data(SMT_START, 
				   service_build_stream_start(t));
    streaming_pad_deliver(&t->s_streaming_pad, sm);
    streaming_msg_free(sm);
  }

  pthread_mutex_unlock(&t->s_stream_mutex);

  if(t->s_refresh_feed != NULL)
    t->s_refresh_feed(t);
}


/**
 * Generate a message containing info about all components
 */
streaming_start_t *
service_build_stream_start(service_t *t)
{
  extern const idclass_t mpegts_service_class;
  elementary_stream_t *st;
  int n = 0;
  streaming_start_t *ss;

  lock_assert(&t->s_stream_mutex);
  
  TAILQ_FOREACH(st, &t->s_components, es_link)
    n++;

  ss = calloc(1, sizeof(streaming_start_t) + 
	      sizeof(streaming_start_component_t) * n);

  ss->ss_num_components = n;
  
  n = 0;
  TAILQ_FOREACH(st, &t->s_components, es_link) {
    streaming_start_component_t *ssc = &ss->ss_components[n++];
    ssc->ssc_index = st->es_index;
    ssc->ssc_type  = st->es_type;

    memcpy(ssc->ssc_lang, st->es_lang, 4);
    ssc->ssc_audio_type = st->es_audio_type;
    ssc->ssc_composition_id = st->es_composition_id;
    ssc->ssc_ancillary_id = st->es_ancillary_id;
    ssc->ssc_pid = st->es_pid;
    ssc->ssc_width = st->es_width;
    ssc->ssc_height = st->es_height;
    ssc->ssc_frameduration = st->es_frame_duration;
  }

  t->s_setsourceinfo(t, &ss->ss_si);

  ss->ss_refcount = 1;
  ss->ss_pcr_pid = t->s_pcr_pid;
  ss->ss_pmt_pid = t->s_pmt_pid;
  if (idnode_is_instance(&t->s_id, &mpegts_service_class)) {
    mpegts_service_t *ts = (mpegts_service_t*)t;
    ss->ss_service_id = ts->s_dvb_service_id;
  }
  return ss;
}


/**
 *
 */

static pthread_mutex_t pending_save_mutex;
static pthread_cond_t pending_save_cond;
static struct service_queue pending_save_queue;

/**
 *
 */
void
service_request_save(service_t *t, int restart)
{
  pthread_mutex_lock(&pending_save_mutex);

  if(!t->s_ps_onqueue) {
    t->s_ps_onqueue = 1 + !!restart;
    TAILQ_INSERT_TAIL(&pending_save_queue, t, s_ps_link);
    service_ref(t);
    pthread_cond_signal(&pending_save_cond);
  } else if(restart) {
    t->s_ps_onqueue = 2; // upgrade to restart too
  }

  pthread_mutex_unlock(&pending_save_mutex);
}


/**
 *
 */
static void
service_class_save(struct idnode *self)
{
  service_t *s = (service_t *)self;
  if (s->s_config_save)
    s->s_config_save(s);
}

/**
 *
 */
static void *
service_saver(void *aux)
{
  service_t *t;
  int restart;
  pthread_mutex_lock(&pending_save_mutex);

  while(tvheadend_running) {

    if((t = TAILQ_FIRST(&pending_save_queue)) == NULL) {
      pthread_cond_wait(&pending_save_cond, &pending_save_mutex);
      continue;
    }
    assert(t->s_ps_onqueue != 0);
    restart = t->s_ps_onqueue == 2;

    TAILQ_REMOVE(&pending_save_queue, t, s_ps_link);
    t->s_ps_onqueue = 0;

    pthread_mutex_unlock(&pending_save_mutex);
    pthread_mutex_lock(&global_lock);

    if(t->s_status != SERVICE_ZOMBIE)
      t->s_config_save(t);
    if(t->s_status == SERVICE_RUNNING && restart) {
      service_restart(t, 1);
    }
    service_unref(t);

    pthread_mutex_unlock(&global_lock);
    pthread_mutex_lock(&pending_save_mutex);
  }
  return NULL;
}


/**
 *
 */
pthread_t service_saver_tid;

void
service_init(void)
{
  TAILQ_INIT(&pending_save_queue);
  TAILQ_INIT(&service_all);
  pthread_mutex_init(&pending_save_mutex, NULL);
  pthread_cond_init(&pending_save_cond, NULL);
  tvhthread_create(&service_saver_tid, NULL, service_saver, NULL, 0);
}

void
service_done(void)
{
  pthread_cond_signal(&pending_save_cond);
  pthread_join(service_saver_tid, NULL);
}

/**
 *
 */
void
service_source_info_free(struct source_info *si)
{
  free(si->si_device);
  free(si->si_adapter);
  free(si->si_network);
  free(si->si_mux);
  free(si->si_provider);
  free(si->si_service);
}


void
service_source_info_copy(source_info_t *dst, const source_info_t *src)
{
#define COPY(x) dst->si_##x = src->si_##x ? strdup(src->si_##x) : NULL
  COPY(device);
  COPY(adapter);
  COPY(network);
  COPY(mux);
  COPY(provider);
  COPY(service);
#undef COPY
}


/**
 *
 */
const char *
service_nicename(service_t *t)
{
  return t->s_nicename;
}

const char *
service_component_nicename(elementary_stream_t *st)
{
  return st->es_nicename;
}

const char *
service_adapter_nicename(service_t *t)
{
  return "Adapter";
}

const char *
service_tss2text(int flags)
{
  if(flags & TSS_NO_ACCESS)
    return "No access";

  if(flags & TSS_NO_DESCRAMBLER)
    return "No descrambler";

  if(flags & TSS_PACKETS)
    return "Got valid packets";

  if(flags & TSS_MUX_PACKETS)
    return "Got multiplexed packets but could not decode further";

  if(flags & TSS_INPUT_SERVICE)
    return "Got packets for this service but could not decode further";

  if(flags & TSS_INPUT_HARDWARE)
    return "Sensed input from hardware but nothing for the service";

  if(flags & TSS_GRACEPERIOD)
    return "No input detected";

  return "No status";
}


/**
 *
 */
int
tss2errcode(int tss)
{
  if(tss & TSS_NO_ACCESS)
    return SM_CODE_NO_ACCESS;

  if(tss & TSS_NO_DESCRAMBLER)
    return SM_CODE_NO_DESCRAMBLER;

  if(tss & TSS_GRACEPERIOD)
    return SM_CODE_NO_INPUT;

  return SM_CODE_OK;
}


/**
 *
 */
void
service_refresh_channel(service_t *t)
{
#if 0
  if(t->s_ch != NULL)
    htsp_channel_update(t->s_ch);
#endif
}


/**
 * Weight then prio?
 */
static int
si_cmp(const service_instance_t *a, const service_instance_t *b)
{
  int r;
  r = a->si_weight - b->si_weight;
  if (!r)
    r = a->si_prio - b->si_prio;
  return r;
}

/**
 *
 */
service_instance_t *
service_instance_add(service_instance_list_t *sil,
                     struct service *s, int instance, int prio,
                     int weight)
{
  service_instance_t *si;

  /* Existing */
  TAILQ_FOREACH(si, sil, si_link)
    if(si->si_s == s && si->si_instance == instance)
      break;

  if(si == NULL) {
    si = calloc(1, sizeof(service_instance_t));
    si->si_s = s;
    service_ref(s);
    si->si_instance = instance;
    si->si_weight = weight;
  } else {
    si->si_mark = 0;
    if(si->si_prio == prio && si->si_weight == weight)
      return si;
    TAILQ_REMOVE(sil, si, si_link);
  }
  si->si_weight = weight;
  si->si_prio   = prio;
  TAILQ_INSERT_SORTED(sil, si, si_link, si_cmp);
  return si;
}


/**
 *
 */
void
service_instance_destroy
  (service_instance_list_t *sil, service_instance_t *si)
{
  TAILQ_REMOVE(sil, si, si_link);
  service_unref(si->si_s);
  free(si);
}


/**
 *
 */
void
service_instance_list_clear(service_instance_list_t *sil)
{
  lock_assert(&global_lock);

  service_instance_t *si;
  while((si = TAILQ_FIRST(sil)) != NULL)
    service_instance_destroy(sil, si);
}

/*
 * Get name for channel from service
 */
const char *
service_get_channel_name ( service_t *s )
{
  const char *r = NULL;
  if (s->s_channel_name) r = s->s_channel_name(s);
  if (!r) r = s->s_nicename;
  return r;
}

/*
 * Get number for service
 */
int
service_get_channel_number ( service_t *s )
{
  if (s->s_channel_number) return s->s_channel_number(s);
  return 0;
}

/**
 * Get the encryption CAID from a service
 * only the first CA stream in a service is returned
 */
uint16_t
service_get_encryption(service_t *t)
{
  elementary_stream_t *st;
  caid_t *c;

  TAILQ_FOREACH(st, &t->s_components, es_link) {
    switch(st->es_type) {
    case SCT_CA:
      LIST_FOREACH(c, &st->es_caids, link)
	if(c->caid != 0)
	  return c->caid;
      break;
    default:
      break;
    }
  }
  return 0;
}

/*
 * Find the primary EPG service (to stop EPG trying to update
 * from multiple OTA sources)
 */
#ifdef MOVE_TO_MPEGTS
int
service_is_primary_epg(service_t *svc)
{
  service_t *ret = NULL, *t;
  if (!svc || !svc->s_ch) return 0;
  LIST_FOREACH(t, &svc->s_ch->ch_services, s_ch_link) {
    if (!t->s_is_enabled(t) || !t->s_dvb_eit_enable) continue;
    if (!ret)
      ret = t;
  }
  return !ret ? 0 : (ret->s_dvb_service_id == svc->s_dvb_service_id);
}
#endif

/*
 * list of known service types
 */
htsmsg_t *servicetype_list ( void )
{
  htsmsg_t *ret;//, *e;
  //int i;
  ret = htsmsg_create_list();
#ifdef TODO_FIX_THIS
 for (i = 0; i < sizeof(stypetab) / sizeof(stypetab[0]); i++ ) {
    e = htsmsg_create_map();
    htsmsg_add_u32(e, "val", stypetab[i].val);
    htsmsg_add_str(e, "str", stypetab[i].str);
    htsmsg_add_msg(ret, NULL, e);
  }
#endif
  return ret;
}

void service_save ( service_t *t, htsmsg_t *m )
{
  elementary_stream_t *st;
  htsmsg_t *list, *sub;

  idnode_save(&t->s_id, m);

  htsmsg_add_u32(m, "pcr", t->s_pcr_pid);
  htsmsg_add_u32(m, "pmt", t->s_pmt_pid);

  pthread_mutex_lock(&t->s_stream_mutex);

  list = htsmsg_create_list();
  TAILQ_FOREACH(st, &t->s_components, es_link) {
    sub = htsmsg_create_map();

    htsmsg_add_u32(sub, "pid", st->es_pid);
    htsmsg_add_str(sub, "type", streaming_component_type2txt(st->es_type));
    htsmsg_add_u32(sub, "position", st->es_position);

    if(st->es_lang[0])
      htsmsg_add_str(sub, "language", st->es_lang);

    if (SCT_ISAUDIO(st->es_type))
      htsmsg_add_u32(sub, "audio_type", st->es_audio_type);

    if(st->es_type == SCT_CA) {
      caid_t *c;
      htsmsg_t *v = htsmsg_create_list();
      LIST_FOREACH(c, &st->es_caids, link) {
	      htsmsg_t *caid = htsmsg_create_map();

	      htsmsg_add_u32(caid, "caid", c->caid);
	      if(c->providerid)
	        htsmsg_add_u32(caid, "providerid", c->providerid);
	      htsmsg_add_msg(v, NULL, caid);
      }

      htsmsg_add_msg(sub, "caidlist", v);
    }

    if(st->es_type == SCT_DVBSUB) {
      htsmsg_add_u32(sub, "compositionid", st->es_composition_id);
      htsmsg_add_u32(sub, "ancillartyid", st->es_ancillary_id);
    }

    if(st->es_type == SCT_TEXTSUB)
      htsmsg_add_u32(sub, "parentpid", st->es_parent_pid);

    if(SCT_ISVIDEO(st->es_type)) {
      if(st->es_width)
	      htsmsg_add_u32(sub, "width", st->es_width);
      if(st->es_height)
	      htsmsg_add_u32(sub, "height", st->es_height);
      if(st->es_frame_duration)
        htsmsg_add_u32(sub, "duration", st->es_frame_duration);
    }
    
    htsmsg_add_msg(list, NULL, sub);
  }
  pthread_mutex_unlock(&t->s_stream_mutex);
  htsmsg_add_msg(m, "stream", list);
}

/**
 *
 */
static int
escmp(const void *A, const void *B)
{
  elementary_stream_t *a = *(elementary_stream_t **)A;
  elementary_stream_t *b = *(elementary_stream_t **)B;
  return a->es_position - b->es_position;
}

/**
 *
 */
void
sort_elementary_streams(service_t *t)
{
  elementary_stream_t *st, **v;
  int num = 0, i = 0;

  TAILQ_FOREACH(st, &t->s_components, es_link)
    num++;

  v = alloca(num * sizeof(elementary_stream_t *));
  TAILQ_FOREACH(st, &t->s_components, es_link)
    v[i++] = st;

  qsort(v, num, sizeof(elementary_stream_t *), escmp);

  TAILQ_INIT(&t->s_components);
  for(i = 0; i < num; i++)
    TAILQ_INSERT_TAIL(&t->s_components, v[i], es_link);
}

/**
 *
 */
static void
add_caid(elementary_stream_t *st, uint16_t caid, uint32_t providerid)
{
  caid_t *c = malloc(sizeof(caid_t));
  c->caid = caid;
  c->providerid = providerid;
  c->delete_me = 0;
  LIST_INSERT_HEAD(&st->es_caids, c, link);
}


/**
 *
 */
static void
load_legacy_caid(htsmsg_t *c, elementary_stream_t *st)
{
  uint32_t a, b;
  const char *v;

  if(htsmsg_get_u32(c, "caproviderid", &b))
    b = 0;

  if(htsmsg_get_u32(c, "caidnum", &a)) {
    if((v = htsmsg_get_str(c, "caid")) != NULL) {
      a = descrambler_name2caid(v);
    } else {
      return;
    }
  }

  add_caid(st, a, b);
}


/**
 *
 */
static void 
load_caid(htsmsg_t *m, elementary_stream_t *st)
{
  htsmsg_field_t *f;
  htsmsg_t *c, *v = htsmsg_get_list(m, "caidlist");
  uint32_t a, b;

  if(v == NULL)
    return;

  HTSMSG_FOREACH(f, v) {
    if((c = htsmsg_get_map_by_field(f)) == NULL)
      continue;
    
    if(htsmsg_get_u32(c, "caid", &a))
      continue;

    if(htsmsg_get_u32(c, "providerid", &b))
      b = 0;

    add_caid(st, a, b);
  }
}

void service_load ( service_t *t, htsmsg_t *c )
{
  htsmsg_t *m;
  htsmsg_field_t *f;
  uint32_t u32, pid;
  elementary_stream_t *st;
  streaming_component_type_t type;
  const char *v;

  idnode_load(&t->s_id, c);

  if(!htsmsg_get_u32(c, "pcr", &u32))
    t->s_pcr_pid = u32;
  if(!htsmsg_get_u32(c, "pmt", &u32))
    t->s_pmt_pid = u32;

  pthread_mutex_lock(&t->s_stream_mutex);
  m = htsmsg_get_list(c, "stream");
  if (m) {
    HTSMSG_FOREACH(f, m) {
      if((c = htsmsg_get_map_by_field(f)) == NULL)
        continue;

      if((v = htsmsg_get_str(c, "type")) == NULL)
        continue;

      type = streaming_component_txt2type(v);
      if(type == -1)
        continue;

      if(htsmsg_get_u32(c, "pid", &pid))
        continue;

      st = service_stream_create(t, pid, type);
    
      if((v = htsmsg_get_str(c, "language")) != NULL)
        strncpy(st->es_lang, lang_code_get(v), 3);

      if (SCT_ISAUDIO(type)) {
        if(!htsmsg_get_u32(c, "audio_type", &u32))
          st->es_audio_type = u32;
      }

      if(!htsmsg_get_u32(c, "position", &u32))
        st->es_position = u32;
   
      load_legacy_caid(c, st);
      load_caid(c, st);

      if(type == SCT_DVBSUB) {
        if(!htsmsg_get_u32(c, "compositionid", &u32))
	        st->es_composition_id = u32;

        if(!htsmsg_get_u32(c, "ancillartyid", &u32))
	        st->es_ancillary_id = u32;
      }

      if(type == SCT_TEXTSUB) {
        if(!htsmsg_get_u32(c, "parentpid", &u32))
	        st->es_parent_pid = u32;
      }

      if(SCT_ISVIDEO(type)) {
        if(!htsmsg_get_u32(c, "width", &u32))
	        st->es_width = u32;

        if(!htsmsg_get_u32(c, "height", &u32))
	        st->es_height = u32;

        if(!htsmsg_get_u32(c, "duration", &u32))
          st->es_frame_duration = u32;
      }
    }
  }
  sort_elementary_streams(t);
  pthread_mutex_unlock(&t->s_stream_mutex);
}
