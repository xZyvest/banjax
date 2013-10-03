/*
 * This class implmentats callbacks function that are called by ATS
 * AUTHORS:
 *   Vmon: May 2013, moving Bill's code to C++
 */
#include <stdio.h>
#include <ts/ts.h>
#include <regex.h>
#include <string.h>

#include <string>
#include <vector>
#include <list>

#include <zmq.hpp>
using namespace std;

#include <re2/re2.h>
//to retrieve the client ip
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

//to run fail2ban-client
#include <stdlib.h>

#include "banjax.h"
#include "banjax_continuation.h"
#include "transaction_muncher.h"
#include "regex_manager.h"
#include "challenge_manager.h"
#include "swabber_interface.h"
#include "ats_event_handler.h"
/*the glabal_cont the global continuation that is generated by
  by the banjax object.*/
//TSCont Banjax::global_contp;
//extern TSMutex Banjax::regex_mutex;


int
ATSEventHandler::banjax_global_eventhandler(TSCont contp, TSEvent event, void *edata)
{
  TSHttpTxn txnp = (TSHttpTxn) edata;
  BanjaxContinuation *cd;

  //raise(SIGINT);
  switch (event) {
  case TS_EVENT_HTTP_READ_REQUEST_HDR:
    TSDebug("banjax", "request" );
    if(contp != Banjax::global_contp) {
      cd = (BanjaxContinuation *) TSContDataGet(contp);
      handle_request(cd);
      return 0;
    } else {
      break;
    }
  case TS_EVENT_HTTP_TXN_START:
    TSDebug("banjax", "txn start" );
	txnp = (TSHttpTxn) edata;
    handle_txn_start(contp, txnp);
    return 0;
  case TS_EVENT_HTTP_TXN_CLOSE:
    TSDebug("banjax", "txn close" );
    txnp = (TSHttpTxn) edata;
    if (contp != Banjax::global_contp) {
      cd = (BanjaxContinuation *) TSContDataGet(contp); 
      TSDebug("banjax", "continuation data being destroyed at %lu", (unsigned long)cd);
      cd->~BanjaxContinuation(); //leave mem manage to ATS
      //TSfree(cd); I think TS is taking care of this
      destroy_continuation(txnp, contp);
    }
    break;
  case TS_EVENT_HTTP_SEND_RESPONSE_HDR:
    TSDebug("banjax", "response" );
    if (contp != Banjax::global_contp) {
      cd = (BanjaxContinuation *) TSContDataGet(contp);
      handle_response(cd);
      return 0;
    } else {
      break;
    }
  case TS_EVENT_TIMEOUT:
    TSDebug("banjax", "timeout" );
    /* when mutex lock is not acquired and continuation is rescheduled,
       the plugin is called back with TS_EVENT_TIMEOUT with a NULL
       edata. We need to decide, in which function did the MutexLock
       failed and call that function again */
    if (contp != Banjax::global_contp) {
      cd = (BanjaxContinuation *) TSContDataGet(contp);
      switch (cd->cf) {
      case BanjaxContinuation::HANDLE_REQUEST:
        handle_request(cd);
        return 0;
      default:
        TSDebug("banjax", "This event was unexpected: %d\n", event);
        break;
      }
    } else {
      //regardless, it even doesn't make sense to read the list here
      //read_regex_list(contp);
      return 0;
    }

  default:
    TSDebug("banjax", "default" );
    break;
  }

  return 0;

}

void
ATSEventHandler::handle_request(BanjaxContinuation* cd)
{

  Banjax* banjax = cd->cur_banjax_inst;

  //retreiving part of header requested by the filters
  const TransactionParts& cur_trans_parts = cd->transaction_muncher.retrieve_parts(banjax->which_parts_are_requested());


  bool continue_filtering = true;
  for(list<BanjaxFilter*>::iterator cur_filter = banjax->filters.begin(); continue_filtering && cur_filter != banjax->filters.end(); cur_filter++) {
    FilterResponse cur_filter_result = (*cur_filter)->execute(cur_trans_parts);
    switch (cur_filter_result.response_type) 
      {
      case FilterResponse::GO_AHEAD_NO_COMMENT:
        continue;
        
      case FilterResponse::NO_WORRIES_SERVE_IMMIDIATELY: //This is when the requester is white listed
        continue_filtering = false;
        break;


      case FilterResponse::I_RESPOND:
        cd->response_info = cur_filter_result;
        cd->responding_filter = *cur_filter;
        cd->response_generator = &BanjaxFilter::generate_response;
        TSHttpTxnHookAdd(cd->txnp, TS_HTTP_SEND_RESPONSE_HDR_HOOK, cd->contp);
        TSHttpTxnReenable(cd->txnp, TS_EVENT_HTTP_ERROR);
        return;

      default:
        //Not implemeneted, hence ignoe
        break;
        
      }
  }
  
  //TODO: it is imaginable that a filter needs to be 
  //called during response but does not need to influnence
  //the response, e.g. botbanger hence we need to get the 
  //response hook while continuing with the flow
  //destroy_continuation(cd->txnp, cd->contp);
  TSHttpTxnReenable(cd->txnp, TS_EVENT_HTTP_CONTINUE);

}

void
ATSEventHandler::handle_response(BanjaxContinuation* cd)
{

  if (cd->response_generator) {
    cd->transaction_muncher.retrieve_response_parts(cd->responding_filter->response_info());
    cd->transaction_muncher.set_status(TS_HTTP_STATUS_FORBIDDEN);
    string alternative_response = ((cd->responding_filter)->*(cd->response_generator))(cd->transaction_muncher.retrieve_parts(cd->cur_banjax_inst->all_filters_requested_part), cd->response_info);
    char* buf = (char *) TSmalloc(alternative_response.length()+1);
    sprintf(buf, "%s", alternative_response.c_str());

    //TSHttpTxnErrorBodySet(cd->txnp, (char*)alternative_response.c_str(), alternative_response.length(), NULL);
    TSHttpTxnErrorBodySet(cd->txnp, buf, strlen(buf), NULL);

  }

  TSHttpTxnReenable(cd->txnp, TS_EVENT_HTTP_CONTINUE);

}

/**
   @param global_contp contains the global continuation and is sent here
   , so the new continuation gets the main banjax object
 */
void
ATSEventHandler::handle_txn_start(TSCont global_contp, TSHttpTxn txnp)
{
  TSCont txn_contp;
  BanjaxContinuation* global_cont_data = (BanjaxContinuation *) TSContDataGet(global_contp);;
  BanjaxContinuation *cd;

  //retreive the banjax obej

  txn_contp = TSContCreate((TSEventFunc) banjax_global_eventhandler, TSMutexCreate());
  /* create the data that'll be associated with the continuation */
  cd = (BanjaxContinuation *) TSmalloc(sizeof(BanjaxContinuation));
  cd = new(cd) BanjaxContinuation(txnp);
  TSDebug("banjax", "New continuation data at %lu", (unsigned long)cd);
  TSContDataSet(txn_contp, cd);

  cd->contp = txn_contp;
  cd->cur_banjax_inst = global_cont_data->cur_banjax_inst;

  TSHttpTxnHookAdd(txnp, TS_HTTP_READ_REQUEST_HDR_HOOK, txn_contp);
  TSHttpTxnHookAdd(txnp, TS_HTTP_TXN_CLOSE_HOOK, txn_contp);

  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
}

void
ATSEventHandler::destroy_continuation(TSHttpTxn txnp, TSCont contp)
{
  BanjaxContinuation *cd = NULL;

  cd = (BanjaxContinuation *) TSContDataGet(contp);
  TSContDestroy(contp);
  TSHttpTxnReenable(cd->txnp, TS_EVENT_HTTP_CONTINUE);

  if (cd != NULL) {
    TSfree(cd);
    // delete cd;
  }
}


