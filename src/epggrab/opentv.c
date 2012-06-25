/*
 *  Electronic Program Guide - opentv epg grabber
 *  Copyright (C) 2012 Adam Sutton
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

#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <linux/dvb/dmx.h>
#include "tvheadend.h"
#include "dvb/dvb.h"
#include "channels.h"
#include "huffman.h"
#include "epg.h"
#include "epggrab/ota.h"
#include "epggrab/opentv.h"
#include "subscriptions.h"
#include "streaming.h"
#include "service.h"
#include "htsmsg.h"
#include "settings.h"

/* ************************************************************************
 * Configuration
 * ***********************************************************************/

#define OPENTV_SCAN_MAX 600    // 10min max scan period
#define OPENTV_SCAN_PER 3600   // 1hour interval


/* Data carousel status */
typedef struct opentv_status
{
  LIST_ENTRY(opentv_status) link;
  int                       pid;
  enum {
    OPENTV_STA_INIT,
    OPENTV_STA_STARTED,
    OPENTV_STA_COMPLETE
  }                         status;
  uint8_t                   start[20];
} opentv_status_t;

/* Huffman dictionary */
typedef struct opentv_dict
{
  char                  *id;
  huffman_node_t        *codes;
  RB_ENTRY(opentv_dict) h_link;
} opentv_dict_t;

/* Provider configuration */
typedef struct opentv_prov
{
  char                  *id;
  char                  *name;
  RB_ENTRY(opentv_prov) h_link;

  int                   nid;
  int                   tsid;
  int                   sid;
  int                   *channel;
  int                   *title;
  int                   *summary;
  opentv_dict_t         *dict;
} opentv_prov_t;

/* Extension of epggrab module to include linked provider */
typedef struct opentv_module
{
  epggrab_module_t   ;      ///< Base struct
  opentv_prov_t      *prov; ///< Associated provider config
  pthread_mutex_t    mutex;
  pthread_cond_t     cond;
  time_t             updated;
  LIST_HEAD(,opentv_status) status;
} opentv_module_t;

/*
 * Lists/Comparators
 */
RB_HEAD(opentv_dict_tree, opentv_dict);
RB_HEAD(opentv_prov_tree, opentv_prov);
struct opentv_dict_tree _opentv_dicts;
struct opentv_prov_tree _opentv_provs;

static int _dict_cmp ( void *a, void *b )
{
  return strcmp(((opentv_dict_t*)a)->id, ((opentv_dict_t*)b)->id);
}

static int _prov_cmp ( void *a, void *b )
{
  return strcmp(((opentv_prov_t*)a)->id, ((opentv_prov_t*)b)->id);
}

static opentv_dict_t *_opentv_dict_find ( const char *id )
{
  opentv_dict_t skel;
  skel.id = (char*)id;
  return RB_FIND(&_opentv_dicts, &skel, h_link, _dict_cmp);
}

/*
 * Configuration loading
 */

static int* _pid_list_to_array ( htsmsg_t *m )
{ 
  int i = 1;
  int *ret;
  htsmsg_field_t *f;
  HTSMSG_FOREACH(f, m) 
    if (f->hmf_s64) i++;
  ret = calloc(i, sizeof(int));
  i   = 0;
  HTSMSG_FOREACH(f, m) 
    if (f->hmf_s64)
      ret[i++] = (int)f->hmf_s64;
  return ret;
}

static int _opentv_dict_load ( const char *id, htsmsg_t *m )
{
  opentv_dict_t *dict = calloc(1, sizeof(opentv_dict_t));
  dict->id = (char*)id;
  if (RB_INSERT_SORTED(&_opentv_dicts, dict, h_link, _dict_cmp)) {
    tvhlog(LOG_WARNING, "opentv", "ignore duplicate dictionary %s", id);
    free(dict);
    return 0;
  } else {
    dict->codes = huffman_tree_build(m);
    if (!dict->codes) {
      RB_REMOVE(&_opentv_dicts, dict, h_link);
      free(dict);
      return -1;
    } else {
      dict->id = strdup(id);
      return 1;
    }
  }
}

static int _opentv_prov_load ( const char *id, htsmsg_t *m )
{
  htsmsg_t *cl, *tl, *sl;
  uint32_t tsid, sid, nid;
  const char *str, *name;
  opentv_dict_t *dict;
  opentv_prov_t *prov;

  /* Check config */
  if (!(name = htsmsg_get_str(m, "name"))) return -1;
  if (!(str  = htsmsg_get_str(m, "dict"))) return -1;
  if (!(dict = _opentv_dict_find(str))) return -1;
  if (!(cl   = htsmsg_get_list(m, "channel"))) return -1;
  if (!(tl   = htsmsg_get_list(m, "title"))) return -5;
  if (!(sl   = htsmsg_get_list(m, "summary"))) return -1;
  if (htsmsg_get_u32(m, "nid", &nid)) return -1;
  if (htsmsg_get_u32(m, "tsid", &tsid)) return -1;
  if (htsmsg_get_u32(m, "sid", &sid)) return -1;

  prov = calloc(1, sizeof(opentv_prov_t));
  prov->id = (char*)id;
  if (RB_INSERT_SORTED(&_opentv_provs, prov, h_link, _prov_cmp)) {
    tvhlog(LOG_WARNING, "opentv", "ignore duplicate provider %s", id);
    free(prov);
    return 0;
  } else {
    prov->id      = strdup(id);
    prov->name    = strdup(name);
    prov->dict    = dict;
    prov->nid     = nid;
    prov->tsid    = tsid;
    prov->sid     = sid;
    prov->channel = _pid_list_to_array(cl);
    prov->title   = _pid_list_to_array(tl);
    prov->summary = _pid_list_to_array(sl);
    return 1;
  }
}

/* ************************************************************************
 * EPG Object wrappers
 * ***********************************************************************/

static epggrab_channel_t *_opentv_find_epggrab_channel
  ( opentv_module_t *mod, int cid, int create, int *save )
{
  char chid[32];
  sprintf(chid, "%s-%d", mod->prov->id, cid);
  return epggrab_module_channel_find((epggrab_module_t*)mod, chid, create, save);
}

static epg_season_t *_opentv_find_season 
  ( opentv_module_t *mod, int cid, int slink )
{
  int save = 0;
  char uri[64];
  sprintf(uri, "%s-%d-%d", mod->prov->id, cid, slink);
  return epg_season_find_by_uri(uri, 1, &save);
}

static service_t *_opentv_find_service ( int tsid, int sid )
{
  th_dvb_adapter_t *tda;
  th_dvb_mux_instance_t *tdmi;
  service_t *t = NULL;
  TAILQ_FOREACH(tda, &dvb_adapters, tda_global_link) {
    LIST_FOREACH(tdmi, &tda->tda_muxes, tdmi_adapter_link) {
      if (tdmi->tdmi_transport_stream_id != tsid) continue;
      LIST_FOREACH(t, &tdmi->tdmi_transports, s_group_link) {
        if (t->s_dvb_service_id == sid) return t;
      }
    }
  }
  return NULL;
}

static channel_t *_opentv_find_channel ( int tsid, int sid )
{
  service_t *t = _opentv_find_service(tsid, sid);
  return t ? t->s_ch : NULL;
}

/* ************************************************************************
 * OpenTV event processing
 * ***********************************************************************/

#define OPENTV_TITLE_LEN   1024
#define OPENTV_SUMMARY_LEN 1024
#define OPENTV_DESC_LEN    2048

#define OPENTV_TITLE       0x01
#define OPENTV_SUMMARY     0x02

/* Internal event structure */
typedef struct opentv_event
{ 
  RB_ENTRY(opentv_event) ev_link;     ///< List of partial events
  uint16_t               cid;         ///< Channel ID
  uint16_t               eid;         ///< Events ID
  time_t                 start;       ///< Start time
  time_t                 stop;        ///< Event stop time
  char                  *title;       ///< Event title
  char                  *summary;     ///< Event summary
  char                  *desc;        ///< Event description
  uint8_t                cat;         ///< Event category
  uint16_t               series;      ///< Series (link) reference
  
  uint8_t                status;      ///< 0x1=title, 0x2=summary
} opentv_event_t;

/* List of partial events */
RB_HEAD(, opentv_event) _opentv_events;

/* Event list comparator */
static int _ev_cmp ( void *_a, void *_b )
{
  opentv_event_t *a = (opentv_event_t*)_a;
  opentv_event_t *b = (opentv_event_t*)_b;
  int r = a->cid - b->cid;
  if (r) return r;
  return a->eid - b->eid;
}

/* Parse huffman encoded string */
static char *_opentv_parse_string 
  ( opentv_prov_t *prov, uint8_t *buf, int len )
{
  int ok = 0;
  char *ret, *tmp;

  // Note: unlikely decoded string will be longer (though its possible)
  ret = tmp = malloc(len+1);
  *ret = 0;
  if (huffman_decode(prov->dict->codes, buf, len, 0x20, tmp, len)) {

    /* Ignore (empty) strings */
    while (*tmp) {
      if (*tmp > 0x20) {
        ok = 1;
        break;
      }
      tmp++;
    }
  }
  if (!ok) {
    free(ret);
    ret = NULL;
  }
  return ret;
}

/* Parse a specific record */
static int _opentv_parse_event_record
 ( opentv_prov_t *prov, opentv_event_t *ev, uint8_t *buf, int len, time_t mjd )
{
  uint8_t rtag = buf[0];
  uint8_t rlen = buf[1];
  if (rlen+2 <= len) {
    switch (rtag) {
      case 0xb5: // title
        ev->start       = (((int)buf[2] << 9) | (buf[3] << 1))
                        + mjd;
        ev->stop        = (((int)buf[4] << 9) | (buf[5] << 1))
                        + ev->start;
        ev->cat         = buf[6];
        if (!ev->title)
          ev->title     = _opentv_parse_string(prov, buf+9, rlen-7);
        break;
      case 0xb9: // summary
        if (!ev->summary)
          ev->summary   = _opentv_parse_string(prov, buf+2, rlen);
        break;
      case 0xbb: // description
        if (!ev->desc)
          ev->desc      = _opentv_parse_string(prov, buf+2, rlen);
        break;
      case 0xc1: // series link
        ev->series      = ((uint16_t)buf[2] << 8) | buf[3];
      break;
      default:
        break;
    }
  }
  return rlen + 2;
}

/* Parse a specific event */
static int _opentv_parse_event
  ( opentv_prov_t *prov, uint8_t *buf, int len, int cid, time_t mjd,
    opentv_event_t **ev )
{
  static opentv_event_t *skel = NULL;
  int      slen = ((int)buf[2] & 0xf << 8) | buf[3];
  int      i    = 4;

  /* Create/Find event entry */
  if (!skel) skel = calloc(1, sizeof(opentv_event_t));
  skel->cid = cid;
  skel->eid = ((uint16_t)buf[0] << 8) | buf[1];
  *ev = RB_INSERT_SORTED(&_opentv_events, skel, ev_link, _ev_cmp);
  if (!*ev) {
    *ev  = skel;
    skel = NULL;
  }

  /* Process records */ 
  if (*ev) {
    while (i < slen+4) {
      i += _opentv_parse_event_record(prov, *ev, buf+i, len-i, mjd);
    }
  }
  return slen+4;
}

/* Parse an event section */
static int _opentv_parse_event_section
  ( opentv_module_t *mod, uint8_t *buf, int len, int type )
{
  int i, cid, save = 0;
  time_t mjd;
  char *uri;
  epggrab_channel_t *ec;
  epg_broadcast_t *ebc;
  epg_episode_t *ee;
  epg_season_t *es;
  opentv_event_t *ev;

  /* Channel */
  cid = ((int)buf[0] << 8) | buf[1];
  if (!(ec = _opentv_find_epggrab_channel(mod, cid, 0, NULL))) return 0;
  if (!ec->channel) return 0;
  if (!*ec->channel->ch_name) return 0; // ignore unnamed channels
  
  /* Time (start/stop referenced to this) */
  mjd = ((int)buf[5] << 8) | buf[6];
  mjd = (mjd - 40587) * 86400;

  /* Loop around event entries */
  i = 7;
  while (i < len) {
    i += _opentv_parse_event(mod->prov, buf+i, len-i, cid, mjd, &ev);

    /* Error */
    if (!ev) continue;

    /* Incomplete */
    ev->status |= type;
    if (ev->status != (OPENTV_TITLE | OPENTV_SUMMARY)) continue;

    /* Find episode */
    uri = epg_hash(ev->title, ev->summary, ev->desc);
    if (uri) {
      ee  = epg_episode_find_by_uri(uri, 1, &save);
      free(uri);
    } else {
      ee  = NULL;
    }

    /* Set episode data */
    if (ee) {
      if (ev->title)
        save |= epg_episode_set_title(ee, ev->title);
      if (ev->summary)
        save |= epg_episode_set_summary(ee, ev->summary);
      if (ev->desc)
        save |= epg_episode_set_description(ee, ev->desc);
      if (ev->cat)
        save |= epg_episode_set_genre(ee, &ev->cat, 1);
      // Note: don't override the season (since the ID is channel specific
      //       it'll keep changing!
      if (ev->series && !ee->season) {
        es = _opentv_find_season(mod, cid, ev->series);
        if (es) save |= epg_episode_set_season(ee, es);
      }

      /* Find broadcast */
      ebc = epg_broadcast_find_by_time(ec->channel, ev->start, ev->stop,
                                       ev->eid, 1, &save);
      if (ebc)
        save |= epg_broadcast_set_episode(ebc, ee);
    }

    /* Remove and cleanup */
    RB_REMOVE(&_opentv_events, ev, ev_link);
    if (ev->title)   free(ev->title);
    if (ev->summary) free(ev->summary);
    if (ev->desc)    free(ev->desc);
    free(ev);
  }

  /* Update EPG */
  if (save) epg_updated();
  return 0;
}

/* ************************************************************************
 * OpenTV channel processing
 * ***********************************************************************/

// TODO: bouqets are ignored, what useful info can we get from them?
static int _opentv_bat_section
  ( opentv_module_t *mod, uint8_t *buf, int len )
{
  epggrab_channel_t *ec;
  int tsid, cid;//, cnum;
  uint16_t sid;
  int i, j, k, tdlen, dlen, dtag, tslen;
  channel_t *ch;
  i     = 7 + ((((int)buf[5] & 0xf) << 8) | buf[6]);
  tslen = (((int)buf[i] & 0xf) << 8) | buf[i+1];
  i    += 2;
  while (tslen > 0) {
    tsid  = ((int)buf[i] << 8) | buf[i+1];
    //nid   = ((int)buf[i+2] << 8) | buf[i+3];
    tdlen = (((int)buf[i+4] & 0xf) << 8) | buf[i+5];
    j      = i + 6;
    i     += (tdlen + 6);
    tslen -= (tdlen + 6);
    while (tdlen > 0) {
      dtag   = buf[j];
      dlen   = buf[j+1];
      k      = j + 2;
      j     += (dlen + 2);
      tdlen -= (dlen + 2);
      if (dtag == 0xb1) {
        k    += 2;
        dlen -= 2;
        while (dlen > 0) {
          sid  = ((int)buf[k] << 8) | buf[k+1];
          cid  = ((int)buf[k+3] << 8) | buf[k+4];
          //cnum = ((int)buf[k+5] << 8) | buf[k+6];

          /* Find the channel */
          ch = _opentv_find_channel(tsid, sid);
          if (ch) {
            int save = 0;
            ec = _opentv_find_epggrab_channel(mod, cid, 1, &save);
            if (save) {
              // Note: could use set_sid() but not nec.
              ec->channel = ch; 
              //TODO: must be configurable
              //epggrab_channel_set_number(ec, cnum);
            }
          }
          k    += 9;
          dlen -= 9;
        }
      }
    }
  }
  return 0;
}

/* ************************************************************************
 * Table Callbacks
 * ***********************************************************************/

static opentv_module_t *_opentv_table_callback 
  ( th_dvb_mux_instance_t *tdmi, uint8_t *buf, int len, uint8_t tid, void *p )
{
  th_dvb_table_t *tdt = (th_dvb_table_t*)p;
  opentv_module_t *mod = (opentv_module_t*)tdt->tdt_opaque;
  epggrab_ota_mux_t *ota;
  opentv_status_t *sta;
  
  /* Ignore (not enough data) */
  if (len < 20) return NULL;

  /* Register */
  ota = epggrab_ota_register((epggrab_module_t*)mod, tdmi,
                             OPENTV_SCAN_MAX, OPENTV_SCAN_PER);
  if (!ota) return NULL;

  /* Finished */
  if (epggrab_ota_is_complete(ota)) return NULL;

  /* Begin */
  if (epggrab_ota_begin(ota)) {
    LIST_FOREACH(sta, &mod->status, link)
      sta->status = OPENTV_STA_INIT;
  }

  /* Insert */
  LIST_FOREACH(sta, &mod->status, link)
    if (sta->pid == tdt->tdt_pid) break;
  if (!sta) {
    sta = calloc(1, sizeof(opentv_status_t));
    LIST_INSERT_HEAD(&mod->status, sta, link);
  }

  /* Set init */
  if (!sta->status) {
    sta->status = OPENTV_STA_STARTED;
    memcpy(sta->start, buf, 20);
    return mod;

  /* Complete */
  } else if (sta->status == 1 && !memcmp(sta->start, buf, 20)) {
    sta->status = OPENTV_STA_COMPLETE;

    /* Check rest */
    LIST_FOREACH(sta, &mod->status, link)
      if (sta->status != OPENTV_STA_COMPLETE) return mod;
  } 

  /* Mark complete */
  epggrab_ota_complete(ota);
  
  return NULL;
}

static int _opentv_title_callback
  ( th_dvb_mux_instance_t *tdmi, uint8_t *buf, int len, uint8_t tid, void *p )
{
  opentv_module_t *mod = _opentv_table_callback(tdmi, buf, len, tid, p);
  if (mod)
    return _opentv_parse_event_section(mod, buf, len, OPENTV_TITLE);
  return 0;
}

static int _opentv_summary_callback
  ( th_dvb_mux_instance_t *tdmi, uint8_t *buf, int len, uint8_t tid, void *p )
{
  opentv_module_t *mod = _opentv_table_callback(tdmi, buf, len, tid, p);
  if (mod)
    return _opentv_parse_event_section(mod, buf, len, OPENTV_SUMMARY);
  return 0;
}

static int _opentv_channel_callback
  ( th_dvb_mux_instance_t *tdmi, uint8_t *buf, int len, uint8_t tid, void *p )
{
  opentv_module_t *mod = _opentv_table_callback(tdmi, buf, len, tid, p);
  if (mod)
    return _opentv_bat_section(mod, buf, len);
  return 0;
}

/* ************************************************************************
 * Module Setup
 * ***********************************************************************/

static epggrab_channel_tree_t _opentv_channels;

static void _opentv_tune ( epggrab_module_t *m, th_dvb_mux_instance_t *tdmi )
{
  int *t;
  struct dmx_sct_filter_params *fp;
  opentv_module_t *mod = (opentv_module_t*)m;
  
  /* Install tables */
  if (m->enabled && (mod->prov->tsid == tdmi->tdmi_transport_stream_id)) {
    tvhlog(LOG_INFO, "opentv", "install provider %s tables", mod->prov->id);

    /* Channels */
    t = mod->prov->channel;
    while (*t) {
      fp = dvb_fparams_alloc();
      fp->filter.filter[0] = 0x4a;
      fp->filter.mask[0]   = 0xff;
      // TODO: what about 0x46 (service description)
      tdt_add(tdmi, fp, _opentv_channel_callback, mod,
              m->id, TDT_CRC | TDT_TDT, *t++, NULL);
    }

    /* Titles */
    t = mod->prov->title;
    while (*t) {
      fp = dvb_fparams_alloc();
      fp->filter.filter[0] = 0xa0;
      fp->filter.mask[0]   = 0xfc;
      tdt_add(tdmi, fp, _opentv_title_callback, mod,
              m->id, TDT_CRC | TDT_TDT, *t++, NULL);
    }

    /* Summaries */
    t = mod->prov->summary;
    while (*t) {
      fp = dvb_fparams_alloc();
      fp->filter.filter[0] = 0xa8;
      fp->filter.mask[0]   = 0xfc;
      tdt_add(tdmi, fp, _opentv_summary_callback, mod,
              m->id, TDT_CRC | TDT_TDT, *t++, NULL);
    }
  }
}

static int _opentv_enable ( epggrab_module_t *m, uint8_t e )
{
  th_dvb_adapter_t *tda;
  th_dvb_mux_instance_t *tdmi;
  opentv_module_t *mod = (opentv_module_t*)m;

  if (m->enabled == e) return 0;

  m->enabled = e;

  /* Find muxes and enable/disable */
  TAILQ_FOREACH(tda, &dvb_adapters, tda_global_link) {
    LIST_FOREACH(tdmi, &tda->tda_muxes, tdmi_adapter_link) {
      if (tdmi->tdmi_transport_stream_id != mod->prov->tsid) continue;
      if (e) {
        epggrab_ota_register(m, tdmi, OPENTV_SCAN_MAX, OPENTV_SCAN_PER);
      } else {
        epggrab_ota_unregister_one(m, tdmi);
      }
    }
  }

  return 1;
}

void opentv_init ( epggrab_module_list_t *list )
{
  int r;
  htsmsg_t *m, *e;
  htsmsg_field_t *f;
  opentv_prov_t *p;
  opentv_module_t *mod;
  char buf[100];

  /* Load the dictionaries */
  if ((m = hts_settings_load("epggrab/opentv/dict"))) {
    HTSMSG_FOREACH(f, m) {
      if ((e = htsmsg_get_list(m, f->hmf_name))) {
        if ((r = _opentv_dict_load(f->hmf_name, e))) {
          if (r > 0) 
            tvhlog(LOG_INFO, "opentv", "dictionary %s loaded", f->hmf_name);
          else
            tvhlog(LOG_WARNING, "opentv", "dictionary %s failed", f->hmf_name);
        }
      }
    }
    htsmsg_destroy(m);
  }
  tvhlog(LOG_INFO, "opentv", "dictonaries loaded");

  /* Load providers */
  if ((m = hts_settings_load("epggrab/opentv/prov"))) {
    HTSMSG_FOREACH(f, m) {
      if ((e = htsmsg_get_map_by_field(f))) {
        if ((r = _opentv_prov_load(f->hmf_name, e))) {
          if (r > 0)
            tvhlog(LOG_INFO, "opentv", "provider %s loaded", f->hmf_name);
          else
            tvhlog(LOG_WARNING, "opentv", "provider %s failed", f->hmf_name);
        }
      }
    }
    htsmsg_destroy(m);
  }
  tvhlog(LOG_INFO, "opentv", "providers loaded");
  
  /* Create modules */
  RB_FOREACH(p, &_opentv_provs, h_link) {
    mod = calloc(1, sizeof(opentv_module_t));
    sprintf(buf, "opentv-%s", p->id);
    mod->id       = strdup(buf);
    sprintf(buf, "OpenTV: %s", p->name);
    mod->name     = strdup(buf);
    mod->enable   = _opentv_enable;
    mod->tune     = _opentv_tune;
    mod->channels = &_opentv_channels;
    mod->prov       = p;
    *((uint8_t*)&mod->flags) = EPGGRAB_MODULE_OTA;
    LIST_INSERT_HEAD(list, ((epggrab_module_t*)mod), link);
  }
}

void opentv_load ( void )
{
  // TODO: do we want to keep a list of channels stored?
}