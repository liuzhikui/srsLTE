
#include <stdlib.h>
#include <stdint.h>

#include "srsapps/ue/mac/mac_params.h"
#include "srsapps/ue/mac/mac_io.h"
#include "srsapps/ue/mac/proc_ra.h"
#include "srsapps/ue/mac/mac.h"
#include "srsapps/ue/mac/mux.h"

/* Random access procedure as specified in Section 5.1 of 36.321 */

namespace srslte {
namespace ue {

// Table 7.2-1. Backoff Parameter values
uint32_t backoff_table[16] = {0, 10, 20, 30, 40, 60, 80, 120, 160, 240, 320, 480, 960, 960, 960, 960};

// Table 7.6-1: DELTA_PREAMBLE values.
int delta_preamble_db_table[5] = {0, 0, -3, -3, 8};

bool ra_proc::init(mac_params* params_db_, phy* phy_h_, srslte::log* log_h_, srslte::timers* timers_db_, 
                   mux* mux_unit_, demux* demux_unit_)
{
  phy_h     = phy_h_; 
  log_h     = log_h_; 
  params_db = params_db_;
  timers_db = timers_db_;
  mux_unit  = mux_unit_; 
  demux_unit= demux_unit_; 
  reset();
}

void ra_proc::reset() {
  state = IDLE;   
}

void ra_proc::read_params() {
  
  // Read initialization parameters   
  configIndex               = params_db->get_param(mac_params::RA_CONFIGINDEX);
  preambleIndex             = params_db->get_param(mac_params::RA_PREAMBLEINDEX);
  maskIndex                 = params_db->get_param(mac_params::RA_MASKINDEX); 
  nof_preambles             = params_db->get_param(mac_params::RA_NOFPREAMBLES); 
  if (!nof_preambles || nof_preambles > 64) {
    nof_preambles = 64; 
  }
  nof_groupA_preambles      = params_db->get_param(mac_params::RA_NOFGROUPAPREAMBLES);
  if (!nof_groupA_preambles) {
    nof_groupA_preambles = nof_preambles; 
  }
  if (nof_groupA_preambles > nof_preambles) {
    nof_groupA_preambles = nof_preambles;
  }
  nof_groupB_preambles      = nof_preambles - nof_groupA_preambles;
  if (nof_groupB_preambles) {
    messagePowerOffsetGroupB  = params_db->get_param(mac_params::RA_MESSAGEPOWEROFFSETB);
    messageSizeGroupA         = params_db->get_param(mac_params::RA_MESSAGESIZEA);
    Pcmax                     = params_db->get_param(mac_params::RA_PCMAX);
    deltaPreambleMsg3         = params_db->get_param(mac_params::RA_DELTAPREAMBLEMSG3);      
  }
  responseWindowSize        = params_db->get_param(mac_params::RA_RESPONSEWINDOW);
  powerRampingStep          = params_db->get_param(mac_params::RA_POWERRAMPINGSTEP);
  preambleTransMax          = params_db->get_param(mac_params::RA_PREAMBLETRANSMAX);
  iniReceivedTargetPower    = params_db->get_param(mac_params::RA_INITRECEIVEDPOWER);
  maxharq_msg3tx            = params_db->get_param(mac_params::RA_MAXTXMSG3);
  contentionResolutionTimer = params_db->get_param(mac_params::RA_CONTENTIONTIMER); 

  delta_preamble_db         = delta_preamble_db_table[configIndex]; 
  
  if (contentionResolutionTimer > 0) {
    timers_db->get(mac::CONTENTION_TIMER)->set(this, contentionResolutionTimer);
  }

}

bool ra_proc::in_progress()
{
  return (state > IDLE && state != COMPLETION);
}

bool ra_proc::is_successful() {
  return state == COMPLETION;
}

bool ra_proc::is_contention_resolution() {
  return state == CONTENTION_RESOLUTION;
}

bool ra_proc::is_error() {
  return state == RA_PROBLEM;
}

uint32_t interval(uint32_t x1, uint32_t x2) {
  if (x1 > x2) {
    return x1-x2; 
  } else {
    return 10240-x2+x1;
  }
}

const char* state_str[10] = {"Idle",
                            "RA Initializat.: ",
                            "RA ResSelection: ",
                            "RA PreambleTx  : ",
                            "RA PreambleRx  : ",
                            "RA ResponseErr : ",
                            "RA BackoffWait : ",
                            "RA ContentResol: ",
                            "RA Completed   : ",
                            "RA Problem     : "};
                           
#define rError(fmt, ...) Error("%s" fmt, state_str[state], ##__VA_ARGS__)                         
#define rInfo(fmt, ...)  Info("%s" fmt, state_str[state], ##__VA_ARGS__)                         
#define rDebug(fmt, ...) Debug("%s" fmt, state_str[state], ##__VA_ARGS__)

                            
// Process Timing Advance Command as defined in Section 5.2
void ra_proc::process_timeadv_cmd(uint32_t tti, uint32_t ta) {
  if (preambleIndex > 0) {
    // Preamble not selected by UE MAC 
    phy_h->set_timeadv_rar(ta);
    timers_db->get(mac::TIME_ALIGNMENT)->reset();
    timers_db->get(mac::TIME_ALIGNMENT)->run();
    Info("Applying RAR TA CMD %d\n", ta);
  } else {
    // Preamble selected by UE MAC 
    if (!timers_db->get(mac::TIME_ALIGNMENT)->is_running()) {
      phy_h->set_timeadv_rar(ta);
      timers_db->get(mac::TIME_ALIGNMENT)->run();
      Info("Applying RAR TA CMD %d\n", ta);
    } else {
      // Ignore TA CMD
      Warning("Ignoring RAR TA CMD because timeAlignmentTimer still running\n");
    }
  }
}

void ra_proc::step_initialization() {
  read_params();
  pdcch_to_crnti_received = PDCCH_CRNTI_NOT_RECEIVED; 
  received_contention_id = 0; 
  transmitted_contention_id = 0; 
  preambleTransmissionCounter = 1; 
  first_rar_received = true; 
  mux_unit->msg3_flush();
  backoff_param_ms = 0; 
  phy_h->init_prach();
  state = RESOURCE_SELECTION;
  rInfo("Done\n");

}

void ra_proc::step_resource_selection() {
  ra_group_t sel_group; 
  if (preambleIndex > 0) {
    // Preamble is chosen by Higher layers (ie Network)
    sel_maskIndex = maskIndex;
    sel_preamble = (uint32_t) preambleIndex%nof_preambles;
  } else {
    // Preamble is chosen by MAC UE
    if (!msg3_transmitted) {
      if (nof_groupB_preambles > 0) { // Check also messageSizeGroupA and pathloss (Pcmax,deltaPreamble and powerOffset)
        sel_group = RA_GROUP_B; 
      } else {
        sel_group = RA_GROUP_A; 
      }
      last_msg3_group = sel_group;
    } else {
      sel_group = last_msg3_group; 
    }
    if (sel_group == RA_GROUP_A) {
      sel_preamble = rand()%(nof_groupA_preambles-1);
    } else {
      sel_preamble = nof_groupA_preambles + rand()%(nof_groupB_preambles-1);
    }
    sel_maskIndex = 0;           
  }
  rInfo("Selected preambleIndex=%d maskIndex=%d nof_GroupApreambles=%d\n", sel_preamble, sel_maskIndex,nof_groupA_preambles);
  state = PREAMBLE_TRANSMISSION;
}

void ra_proc::step_preamble_transmission() {
  received_target_power_dbm = iniReceivedTargetPower + 
      delta_preamble_db + 
      (preambleTransmissionCounter-1)*powerRampingStep;
      
  phy_h->send_prach(sel_preamble, sel_maskIndex - 1, received_target_power_dbm); 
  rInfo("Selected received_target_power_dbm=%d dBm\n", received_target_power_dbm);
  state = RESPONSE_RECEPTION;
}

void ra_proc::step_response_reception() {
  int ra_tti = phy_h->get_prach_transmitted_tti();
  if (ra_tti > 0) {
    
    ra_rnti = 1+ra_tti%10; // f_id=0 for FDD 
    dl_sched_grant rar_grant(ra_rnti); 
    uint32_t interval_ra = interval(tti, ra_tti);
    
    // Try to decode RAR only within the RA response window
    if (interval_ra >= 3 && interval_ra <= 3+responseWindowSize) {        

      // Get DL grant for RA-RNTI
      dl_buffer *dl_buffer = phy_h->get_dl_buffer(tti); 
      if (dl_buffer->get_dl_grant(&rar_grant)) 
      {            
        rInfo("DL grant found RA-RNTI=%d\n", ra_rnti);
        
        if (rar_grant.get_tbs() > MAX_RAR_PDU_LEN) {
          rError("RAR PDU exceeds local RAR PDU buffer (%d>%d)\n", rar_grant.get_tbs(), MAX_RAR_PDU_LEN);
          state = RESPONSE_ERROR;
          return; 
        }
       
        // Decode packet
        if (dl_buffer->decode_data(&rar_grant, rar_pdu_buffer)) {
          rar_pdu.init(rar_grant.get_tbs());
          
          // Set Backoff parameter
          if (rar_pdu.is_backoff()) {
            backoff_param_ms = backoff_table[rar_pdu.get_backoff()%16];
          } else {
            backoff_param_ms = 0; 
          }
          
          while(rar_pdu.read_next()) {
            if (rar_pdu.get()->get_rapid() == sel_preamble) {
              rInfo("Received RAPID=%d\n", sel_preamble);

              process_timeadv_cmd(tti, rar_pdu.get()->get_ta_cmd());
              
              // FIXME: Indicate received target power
              //phy_h->set_target_power_rar(iniReceivedTargetPower, (preambleTransmissionCounter-1)*powerRampingStep);

              // Indicate grant to PHY layer. RAR grants have 6 sf delay (4 is the default delay)
              uint8_t grant[mac_rar_pdu::mac_rar::RAR_GRANT_LEN];
              rar_pdu.get()->get_sched_grant(grant);
              phy_h->get_dl_buffer(tti+2)->set_rar_grant(grant);
              
              if (preambleIndex > 0) {
                // Preamble selected by Network
                state = COMPLETION; 
              } else {
                // Preamble selected by UE MAC 
                params_db->set_param(mac_params::RNTI_TEMP, rar_pdu.get()->get_temp_crnti());
                if (first_rar_received) {
                  first_rar_received = false; 
                  
                  // Save transmitted C-RNTI (if any) 
                  transmitted_crnti = params_db->get_param(mac_params::RNTI_C);
                  
                  // Save transmitted UE contention id, as defined by higher layers 
                  transmitted_contention_id = params_db->get_param(mac_params::CONTENTION_ID);
                  params_db->set_param(mac_params::CONTENTION_ID, 0);                        

                  // If we have a C-RNTI, tell Mux unit to append C-RNTI CE if no CCCH SDU transmission
                  if (transmitted_crnti) {
                    mux_unit->append_crnti_ce_next_tx(transmitted_crnti);
                  } 
                  
                  // Get TransportBlock size for the grant
                  ul_sched_grant msg3_grant(rar_pdu.get()->get_temp_crnti());
                  phy_h->get_dl_buffer(tti+2)->get_ul_grant(&msg3_grant);
                  
                  // Move MAC PDU from Multiplexing and assembly unit to Msg3 
                  mux_unit->pdu_move_to_msg3(tti, msg3_grant.get_tbs()); // 56 is the minimum grant provided 
                  
                  state = CONTENTION_RESOLUTION;
                  
                  // Start contention resolution timer 
                  timers_db->get(mac::CONTENTION_TIMER)->reset();
                  timers_db->get(mac::CONTENTION_TIMER)->run();                      
                }
              }  
            }
          }         
        }        
      }
    }
    if (interval_ra > 3+responseWindowSize && interval_ra < 10000) {
      rInfo("Timeout while trying to receive RAR\n");
      state = RESPONSE_ERROR; 
    }
  }
}

void ra_proc::step_response_error() {
  mux_unit->msg3_flush();
  
  preambleTransmissionCounter++;
  if (preambleTransmissionCounter == preambleTransMax + 1) {
    rError("Maximum number of transmissions reached (%d)\n", preambleTransMax);
    state = RA_PROBLEM;
  } else {
    backoff_interval_start = tti; 
    if (backoff_param_ms) {
      backoff_inteval = rand()%backoff_param_ms;          
    } else {
      backoff_inteval = 0; 
    }
    if (backoff_inteval) {
      rInfo("Backoff wait interval %d\n", backoff_inteval);
      state = BACKOFF_WAIT; 
    } else {
      rInfo("Transmitting inmediatly\n");
      state = RESOURCE_SELECTION;
    }
  }
}

void ra_proc::step_backoff_wait() {
  if (interval(tti, backoff_interval_start) >= backoff_inteval) {
    state = RESOURCE_SELECTION; 
  }
}

void ra_proc::step_contention_resolution() {
  // If Msg3 has been sent
  if (mux_unit->msg3_isempty()) {          
    msg3_transmitted = true; 
    if (pdcch_to_crnti_received != PDCCH_CRNTI_NOT_RECEIVED) 
    {
      // Random Access initiated by MAC itself or PDCCH order (transmission of MAC C-RNTI CE)
      if (start_mode == MAC_ORDER && pdcch_to_crnti_received == PDCCH_CRNTI_UL_GRANT ||
          start_mode == PDCCH_ORDER) 
      {
        timers_db->get(mac::CONTENTION_TIMER)->stop();
        params_db->set_param(mac_params::RNTI_TEMP, 0);
        state = COMPLETION;           
      }            
      pdcch_to_crnti_received = PDCCH_CRNTI_NOT_RECEIVED;
      
    } else if (demux_unit->is_temp_crnti_pending()) 
    {
      // Random Access initiated by RRC by the transmission of CCCH SDU
      received_contention_id = demux_unit->get_contention_resolution_id();
      if (received_contention_id) {
        // MAC PDU successfully decoded and contains MAC CE contention Id
        if (transmitted_contention_id == received_contention_id) {
          
          // UE Contention Resolution ID included in MAC CE matches the CCCH SDU transmitted in Msg3
          timers_db->get(mac::CONTENTION_TIMER)->stop();
          params_db->set_param(mac_params::RNTI_C, params_db->get_param(mac_params::RNTI_TEMP));
          
          // finish the disassembly and demultiplexing of the MAC PDU
          demux_unit->demultiplex_pending_pdu(tti);
          state = COMPLETION;                           
        } else {
          // Discard MAC PDU 
          state = RESPONSE_ERROR; 
        }
        params_db->set_param(mac_params::RNTI_TEMP, 0);              
      }
    }  
  }
}

void ra_proc::step_completition() {
  params_db->set_param(mac_params::RA_PREAMBLEINDEX, 0);
  params_db->set_param(mac_params::RA_MASKINDEX, 0);
  mux_unit->msg3_flush();
  msg3_transmitted = false; 
}

void ra_proc::step(uint32_t tti_)
{
  tti = tti_;
  if (is_running()) {
    switch(state) {
      case IDLE: 
        break;
      case INITIALIZATION:
        step_initialization();
        break;
      case RESOURCE_SELECTION:
        step_resource_selection();
      break;
      case PREAMBLE_TRANSMISSION:
        step_preamble_transmission();
      break;
      case RESPONSE_RECEPTION:      
        step_response_reception();
      break;
      case RESPONSE_ERROR:
        step_response_error();
      break;
      case BACKOFF_WAIT:
        step_backoff_wait();
      break;
      case CONTENTION_RESOLUTION:
        step_contention_resolution();
      break;
      case COMPLETION:
        step_completition();
      break;
    }
  }  
}

void ra_proc::start_mac_order()
{
  if (state == IDLE || state == COMPLETION || state == RA_PROBLEM) {
    start_mode = MAC_ORDER;
    state = INITIALIZATION;    
  }
}

void ra_proc::start_pdcch_order()
{
  if (state == IDLE || state == COMPLETION || state == RA_PROBLEM) {
    start_mode = PDCCH_ORDER;
    state = INITIALIZATION;    
  }
}

void ra_proc::start_rlc_order()
{
  if (state == IDLE || state == COMPLETION || state == RA_PROBLEM) {
    start_mode = RLC_ORDER;
    state = INITIALIZATION;    
  }
}

// Contention Resolution Timer is expired (Section 5.1.5)
void ra_proc::timer_expired(uint32_t timer_id)
{
  params_db->set_param(mac_params::RNTI_TEMP, 0);
  state = RESPONSE_ERROR; 
}

void ra_proc::pdcch_to_crnti(bool is_uplink_grant) {
  if (is_uplink_grant) {
    pdcch_to_crnti_received = PDCCH_CRNTI_UL_GRANT;     
  } else {
    pdcch_to_crnti_received = PDCCH_CRNTI_DL_GRANT;         
  }
}

}
}
