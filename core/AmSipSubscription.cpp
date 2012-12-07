/*
 * Copyright (C) 2012 Frafos GmbH
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. This program is released under
 * the GPL with the additional exemption that compiling, linking,
 * and/or using OpenSSL is allowed.
 *
 * For a license to use the SEMS software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact iptel.org by e-mail at the following addresses:
 *    info@iptel.org
 *
 * SEMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "AmSipSubscription.h"
#include "AmBasicSipDialog.h"
#include "AmEventQueue.h"
#include "AmSipHeaders.h"
#include "AmAppTimer.h"
#include "AmUtils.h"

#include "sip/sip_timers.h"
#include "log.h"

#include <assert.h>

#define RFC6665_TIMER_N_DURATION (64*T1_TIMER)/1000.0

#define SIP_HDR_SUBSCRIPTION_STATE "Subscription-State"
#define SIP_HDR_EVENT              "Event"

const char* __timer_id_str[2] = {
  "RFC6665_TIMER_N",
  "SUBSCRIPTION_EXPIRE" 
};

const char* __sub_state_str[] = {
  "init",
  "notify_wait",
  "pending",
  "active",
  "terminated"
};


/**
 * \brief Single SIP Subscription
 *
 * This class contain only one SIP suscription,
 * identified by its event package name, id and role.
 */
class SingleSubscription 
{
public:
  enum Role {
    Subscriber=0,
    Notifier
  };

private:
  class SubscriptionTimer
    : public DirectAppTimer
  {
    SingleSubscription* sub;
    int timer_id;
    
  public:
    SubscriptionTimer(SingleSubscription* sub, int timer_id)
      : sub(sub), timer_id(timer_id)
    {}
    
    void fire(){
      sub->onTimer(timer_id);
    }
  };

  enum SubscriptionTimerId {
    RFC6665_TIMER_N=0,
    SUBSCRIPTION_EXPIRE
  };

  // state
  unsigned int sub_state;
  AmMutex  sub_state_mut;
  int  pending_subscribe;

  // timers
  SubscriptionTimer timer_n;
  SubscriptionTimer timer_expires;

  AmSipSubscription* subs;

  SingleSubscription(AmSipSubscription* subs, Role role,
		     const string& event, const string& id)
    : subs(subs), role(role), event(event), id(id),
      sub_state(SubState_init), pending_subscribe(0),
      timer_n(this,RFC6665_TIMER_N),timer_expires(this,SUBSCRIPTION_EXPIRE)
  { 
    assert(subs); 
  }

  void onTimer(int timer_id);

  AmBasicSipDialog* dlg() 
  {
    return subs->dlg;
  }

  void requestFSM(const AmSipRequest& req);
  
  friend class SubscriptionTimer;

public:  
  enum SubscriptionState {
    SubState_init=0,
    SubState_notify_wait,
    SubState_pending,
    SubState_active,
    SubState_terminated
  };
  
  // identifiers
  string event;
  string    id;
  Role    role;

  static SingleSubscription* makeSubscription(AmSipSubscription* subs,
					      const AmSipRequest& req,
					      bool uac);

  ~SingleSubscription();

  bool onRequestIn(const AmSipRequest& req);
  void onRequestSent(const AmSipRequest& req);
  void replyFSM(const AmSipRequest& req, const AmSipReply& reply);

  unsigned int getState() { return sub_state; }
  void setState(unsigned int st);
  void lockState() { sub_state_mut.lock(); }
  void unlockState() { sub_state_mut.unlock(); }

  void terminate();
  bool terminated();
};

void SingleSubscription::onTimer(int timer_id)
{
  DBG("[%p] tag=%s;role=%s timer_id = %s\n",this,
      role ? "Notifier" : "Subscriber",
      dlg()->local_tag.c_str(),
      __timer_id_str[timer_id]);

  switch(timer_id){
  case RFC6665_TIMER_N:
  case SUBSCRIPTION_EXPIRE:
    lockState();
    terminate();
    unlockState();
    if(subs->ev_q)
      subs->ev_q->postEvent(NULL);
    return;
  }  
}

void SingleSubscription::terminate()
{
  setState(SubState_terminated);
}

bool SingleSubscription::terminated()
{
  return getState() == SubState_terminated;
}

SingleSubscription* SingleSubscription::makeSubscription(AmSipSubscription* subs,
							 const AmSipRequest& req,
							 bool uac)
{
  Role role = uac ? Subscriber : Notifier;
  string event;
  string id;

  if(req.method == SIP_METH_SUBSCRIBE) {
    // fetch Event-HF
    event = getHeader(req.hdrs,SIP_HDR_EVENT,true);
    id = get_header_param(event,"id");
    event = strip_header_params(event);
  }
  else if(req.method == SIP_METH_REFER) {
    //TODO: fetch Refer-Sub-HF (RFC 4488)
    event = "refer";
    id = int2str(req.cseq);
  } 
  else {
    DBG("subscription are only created by SUBSCRIBE or REFER requests\n");
    // subscription are only created by SUBSCRIBE or REFER requests
    // and we do not support unsolicited NOTIFYs
    return NULL;
  }

  return new SingleSubscription(subs,role,event,id);
}

SingleSubscription::~SingleSubscription()
{
  // just to be sure...
  AmAppTimer::instance()->removeTimer(&timer_n);
  // this one should still be active
  AmAppTimer::instance()->removeTimer(&timer_expires);
}

void SingleSubscription::requestFSM(const AmSipRequest& req)
{
  if((req.method == SIP_METH_SUBSCRIBE) ||
     (req.method == SIP_METH_REFER)) {

    lockState();
    if(getState() == SubState_init) {
      setState(SubState_notify_wait);
    }
    unlockState();

    // start Timer N (RFC6665/4.1.2)
    DBG("setTimer(%s,RFC6665_TIMER_N)\n",dlg()->local_tag.c_str());
    AmAppTimer::instance()->setTimer(&timer_n,RFC6665_TIMER_N_DURATION);
  }
}

bool SingleSubscription::onRequestIn(const AmSipRequest& req)
{
  if((req.method == SIP_METH_SUBSCRIBE) ||
     (req.method == SIP_METH_REFER)) {

    if(pending_subscribe) {
      dlg()->reply(req,500, SIP_REPLY_SERVER_INTERNAL_ERROR, NULL,
		   SIP_HDR_COLSP(SIP_HDR_RETRY_AFTER) 
		   + int2str(get_random() % 10) + CRLF);
      return false;
    }
    pending_subscribe++;
  }

  requestFSM(req);
  return true;
}

void SingleSubscription::onRequestSent(const AmSipRequest& req)
{
  //TODO: check pending_subscribe in onSendRequest
  if((req.method == SIP_METH_SUBSCRIBE) ||
     (req.method == SIP_METH_REFER)) {
    pending_subscribe++;
  }
  requestFSM(req);
}

void SingleSubscription::replyFSM(const AmSipRequest& req, const AmSipReply& reply)
{
  if(reply.code < 200)
    return;

  if((req.method == SIP_METH_SUBSCRIBE) ||
     (req.method == SIP_METH_REFER)) {
    // final reply

    if(reply.code >= 300) {
      lockState();
      if(getState() == SubState_notify_wait) {
	// initial SUBSCRIBE failed
	terminate();
      }
      else {
	// subscription refresh failed
	// from RFC 5057: terminate usage
	switch(reply.code){
	case 405:
	case 489:
	case 481:
	case 501:
	  terminate();
	  break;
	}
      }
      unlockState();
    }
    else {
      // success
      
      // set dialog identifier if not yet set
      if(dlg()->remote_tag.empty()) {
	dlg()->updateRemoteTag(reply.to_tag);
	dlg()->updateRouteSet(reply.route);
      }

      // check Expires-HF
      string expires_txt = getHeader(reply.hdrs,SIP_HDR_EXPIRES,true);
      expires_txt = strip_header_params(expires_txt);

      int sub_expires=0;
      if(!expires_txt.empty() && str2int(expires_txt,sub_expires)){
	if(sub_expires){
	  DBG("setTimer(%s,SUBSCRIPTION_EXPIRE)\n",dlg()->local_tag.c_str());
	  AmAppTimer::instance()->setTimer(&timer_expires,(double)sub_expires);
	}
	else {
	  // we do not care too much, as timer N is set
	  // for each SUBSCRIBE request
	  DBG("Expires-HF equals 0\n");
	}
      }
      else if(reply.cseq_method == SIP_METH_SUBSCRIBE){
	// Should we really enforce that?
	// -> we still have timer N...

	// replies to SUBSCRIBE MUST contain a Expires-HF
	// if not, or if not readable, we should probably 
	// quit the subscription
	DBG("replies to SUBSCRIBE MUST contain a Expires-HF\n");
	lockState();
	terminate();
	unlockState();
      }
    }

    pending_subscribe--;
  }
  else if(reply.cseq_method == SIP_METH_NOTIFY) {

    if(reply.code >= 300) {
      // final error reply
      // from RFC 5057: terminate usage
      switch(reply.code){
      case 405:
      case 481:
      case 489:
      case 501:
	lockState();
	terminate();
	unlockState();
	break;
	
      default:
	// all other response codes:
	// only the transaction fails
	break;
      }

      return;
    }
    
    // check Subscription-State-HF
    string sub_state_txt = getHeader(req.hdrs,SIP_HDR_SUBSCRIPTION_STATE,true);
    string expires_txt = get_header_param(sub_state_txt,"expires");
    int notify_expire=0;
  
    if(!expires_txt.empty())
      str2int(expires_txt,notify_expire);

    sub_state_txt = strip_header_params(sub_state_txt);
    if(notify_expire && (sub_state_txt == "active")) {
      lockState();
      setState(SubState_active);
      unlockState();
    }
    else if(notify_expire && (sub_state_txt == "pending")){
      lockState();
      setState(SubState_pending);
      unlockState();
    }
    else {
      lockState();
      terminate();
      unlockState();
      // there is probably more to do than
      // just ignoring the request... but what?
      return;
    }
    
    // Kill timer N
    DBG("removeTimer(%s,RFC6665_TIMER_N)\n",dlg()->local_tag.c_str());
    AmAppTimer::instance()->removeTimer(&timer_n);
    // reset expire timer
    DBG("setTimer(%s,SUBSCRIPTION_EXPIRE)\n",dlg()->local_tag.c_str());
    AmAppTimer::instance()->setTimer(&timer_expires,(double)notify_expire);
  }

  return;
}

void SingleSubscription::setState(unsigned int st)
{
  DBG("st = %s\n",__sub_state_str[st]);

  if(sub_state == SubState_terminated)
    return;

  if(st == SubState_terminated) {
    sub_state = SubState_terminated;
    dlg()->decUsages();
  }
  else {
    sub_state = st;
  }
}

/**
 * AmSipSubscription
 */

AmSipSubscription::AmSipSubscription(AmBasicSipDialog* dlg, AmEventQueue* ev_q)
  : dlg(dlg), ev_q(ev_q)
{
  assert(dlg);
}

AmSipSubscription::~AmSipSubscription()
{
  for(Subscriptions::iterator it=subs.begin();
      it != subs.end(); it++) {
    delete *it;
  }
}

void AmSipSubscription::terminate()
{
  for(Subscriptions::iterator it=subs.begin();
      it != subs.end(); it++) {
    (*it)->lockState();
    (*it)->terminate();
    (*it)->unlockState();
  }
}

AmSipSubscription::Subscriptions::iterator
AmSipSubscription::createSubscription(const AmSipRequest& req, bool uac)
{
  SingleSubscription* sub = SingleSubscription::makeSubscription(this,req,uac);
  if(!sub){
    dlg->reply(req, 501, "NOTIFY cannot create a subscription");
    return subs.end();
  }

  dlg->incUsages();
  return subs.insert(subs.end(),sub);
}

/**
 * match single subscription
 * if none, create one
 */
AmSipSubscription::Subscriptions::iterator
AmSipSubscription::matchSubscription(const AmSipRequest& req, bool uac)
{
  if(dlg->remote_tag.empty() || (req.method == SIP_METH_REFER) || subs.empty()) {
    DBG("no to-tag, REFER or subs empty: create new subscription\n");
    return createSubscription(req,uac);
  }

  SingleSubscription::Role role;
  string event;
  string id;

  if(req.method == SIP_METH_SUBSCRIBE) {
    role = uac ? SingleSubscription::Subscriber : SingleSubscription::Notifier;
  }
  else if(req.method == SIP_METH_NOTIFY){
    role = uac ? SingleSubscription::Notifier : SingleSubscription::Subscriber;
  }
  else {
    DBG("unsupported request\n");
    return subs.end();
  }

  // parse Event-HF
  event = getHeader(req.hdrs,SIP_HDR_EVENT,true);
  id = get_header_param(event,"id");
  event = strip_header_params(event);

  Subscriptions::iterator match = subs.end();
  bool no_id = id.empty() && (event == "refer");

  for(Subscriptions::iterator it = subs.begin();
      it != subs.end(); it++) {

    SingleSubscription* sub = *it;
    if( (sub->role == role) &&
	(sub->event == event) &&
	(no_id || (sub->id == id)) ){

      match = it;
      break;
    }
  }

  if((match != subs.end()) && (*match)->terminated()) {
    DBG("matched terminated subscription: deleting it first\n");
    delete *match;
    subs.erase(match);
    match = subs.end();
  }

  if(match == subs.end()){
    if(req.method == SIP_METH_SUBSCRIBE) {
      // no match... new subscription?
      DBG("no match found, SUBSCRIBE: create new subscription\n");
      return createSubscription(req,uac);
    }
  }

  return match;
}

bool AmSipSubscription::onRequestIn(const AmSipRequest& req)
{
  // UAS side
  Subscriptions::iterator sub_it = matchSubscription(req,false);
  if((sub_it == subs.end()) || (*sub_it)->terminated()) {
    dlg->reply(req, 481, SIP_REPLY_NOT_EXIST);
    return false;
  }
  
  // process request;
  uas_cseq_map[req.cseq] = sub_it;
  return (*sub_it)->onRequestIn(req);
}

void AmSipSubscription::onRequestSent(const AmSipRequest& req)
{
  // UAC side
  Subscriptions::iterator sub_it = matchSubscription(req,true);
  if(sub_it == subs.end()){
    // should we exclude this case in onSendRequest???
    ERROR("we just sent a request for which we could obtain no subscription\n");
    return;
  }

  // process request;
  uac_cseq_map[req.cseq] = sub_it;
  (*sub_it)->onRequestSent(req);
}

bool AmSipSubscription::onReplyIn(const AmSipRequest& req,
				  const AmSipReply& reply)
{
  // UAC side
  CSeqMap::iterator cseq_it = uac_cseq_map.find(req.cseq);
  if(cseq_it == uac_cseq_map.end()){
    DBG("could not find %i in our uac_cseq_map\n",req.cseq);
    return false;
  }

  Subscriptions::iterator sub_it = cseq_it->second;
  SingleSubscription* sub = *sub_it;
  uac_cseq_map.erase(cseq_it);

  sub->replyFSM(req,reply);
  if(sub->terminated()){
    subs.erase(sub_it);
    delete sub;
  }

  return true;
}

void AmSipSubscription::onReplySent(const AmSipRequest& req,
				    const AmSipReply& reply)
{
  // UAS side
  CSeqMap::iterator cseq_it = uas_cseq_map.find(req.cseq);
  if(cseq_it == uas_cseq_map.end())
    return;

  Subscriptions::iterator sub_it = cseq_it->second;
  SingleSubscription* sub = *sub_it;
  uas_cseq_map.erase(cseq_it);

  sub->replyFSM(req,reply);
  if(sub->terminated()){
    subs.erase(sub_it);
    delete sub;
  }
}
