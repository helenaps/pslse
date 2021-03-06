#include "AFU.h"

#include <string>
#include <stdio.h>
#include <iostream>
#include <stdlib.h>
#include <time.h>

using std::string;
using std::cout;
using std::endl;
using std::vector;

#define GLOBAL_CONFIG_OFFSET 0x400
#define CONTEXT_SIZE 0x400
#define CONTEXT_MASK (CONTEXT_SIZE - 1)

AFU::AFU (int port, string filename, bool parity):
    descriptor (filename),
    context_to_mc ()
{

    // initializes AFU socket connection as server
    if (psl_serv_afu_event (&afu_event, port) == PSL_BAD_SOCKET)
        error_msg ("AFU: unable to create socket");

    if (psl_afu_aux2_change
            (&afu_event, afu_event.job_running, afu_event.job_done,
             afu_event.job_cack_llcmd, afu_event.job_error, afu_event.job_yield,
             afu_event.timebase_request, (int) parity, 1) != PSL_SUCCESS) {
        error_msg ("AFU: failed to set parity_enable and latency");
    }

    set_seed ();

    state = IDLE;

    reset ();
}

void
AFU::start ()
{
    uint32_t cycle = 0;

    while (1) {
        fd_set watchset;

        FD_ZERO (&watchset);
        FD_SET (afu_event.sockfd, &watchset);
        select (afu_event.sockfd + 1, &watchset, NULL, NULL, NULL);
        int rc = psl_get_psl_events (&afu_event);

        //info_msg("Cycle: %d", cycle);
        ++cycle;

        if (rc < 0) {		// connection dropped
            info_msg ("AFU: connection lost");
            break;
        }

        if (rc <= 0)		// no events to be processed
            continue;

        // job done should only be asserted for one cycle
        if (afu_event.job_done)
            afu_event.job_done = 0;

        // process event
        if (afu_event.job_valid == 1) {
            debug_msg ("AFU: Received control event");
            resolve_control_event ();
            afu_event.job_valid = 0;
        }

        if (afu_event.response_valid == 1) {
            if (state != RUNNING && state != WAITING_FOR_LAST_RESPONSES
                    && state != RESET) {
                error_msg
                ("AFU: received response event when AFU is not running");
            }
            debug_msg ("AFU: Received response event");
            resolve_response_event (cycle);
            afu_event.response_valid = 0;
        }

        if (afu_event.mmio_valid == 1) {
            if (afu_event.mmio_double && (afu_event.mmio_address & 0x1))
                error_msg ("AFU: mmio double access on non-even address");

            if (afu_event.mmio_afudescaccess) {
                if (state == IDLE || state == RESET) {
                    error_msg
                    ("AFU: Error MMIO descriptor access before AFU is done resetting");
                }
                debug_msg ("AFU: Received MMIO descriptor event");
                resolve_mmio_descriptor_event ();
            }
            else {
                if (state != RUNNING && state != WAITING_FOR_LAST_RESPONSES) {
                    error_msg
                    ("AFU: received MMIO non-descriptor access when AFU is not running");
                }
                debug_msg ("AFU: Received MMIO non-descriptor event");
                resolve_mmio_event ();
            }
            afu_event.mmio_valid = 0;
        }

        if (afu_event.buffer_write == 1) {
            if (state != RUNNING && state != WAITING_FOR_LAST_RESPONSES
                    && state != RESET) {
                error_msg
                ("AFU: received buffer write when AFU is not running");
            }
            debug_msg ("AFU: Received buffer write event");
            resolve_buffer_write_event ();
            afu_event.buffer_write = 0;
        }

        if (afu_event.buffer_read == 1) {
            if (state != RUNNING && state != WAITING_FOR_LAST_RESPONSES
                    && state != RESET) {
                error_msg
                ("AFU: received buffer read event when AFU is not running");
            }
            debug_msg ("AFU: Received buffer read event");
            resolve_buffer_read_event ();
            afu_event.buffer_read = 0;
        }

        if (afu_event.aux1_change == 1) {
            debug_msg ("AFU: aux1 change");
            resolve_aux1_event ();
            afu_event.aux1_change = 0;
        }

        // generate commands
        if (state == RUNNING) {
            if (context_to_mc.size () != 0) {
                std::map < uint16_t, MachineController * >::iterator prev =
                    highest_priority_mc;
                do {
                    if (highest_priority_mc == context_to_mc.end ())
                        highest_priority_mc = context_to_mc.begin ();

                    if (highest_priority_mc->
                            second->send_command (&afu_event, cycle)) {
                        debug_msg ("AFU: context %d sent command",
                                   highest_priority_mc->first);
                        ++highest_priority_mc;
                        break;
                    }
                    //++highest_priority_mc;
                } while (++highest_priority_mc != prev);
            }
        }
        else if (state == RESET) {
            if (reset_delay == 0) {
                state = READY;
                reset ();
                debug_msg ("AFU: sending job_done after reset");

                if (psl_afu_aux2_change
                        (&afu_event, afu_event.job_running, 1,
                         afu_event.job_cack_llcmd, afu_event.job_error,
                         afu_event.job_yield, afu_event.timebase_request,
                         afu_event.parity_enable,
                         afu_event.buffer_read_latency) != PSL_SUCCESS) {
                    error_msg ("AFU: failed to assert job_done");
                }
            }
            else {
                //debug_msg("AFU reset delay %d", reset_delay);
                if (reset_delay > 0)
                    --reset_delay;
            }
        }
        else if (state == WAITING_FOR_LAST_RESPONSES) {
            //debug_msg("AFU: waiting for last responses");
            bool all_machines_completed = true;

            for (std::map < uint16_t, MachineController * >::iterator it =
                        context_to_mc.begin (); it != context_to_mc.end (); ++it)
            {
                if (!(it->second)->all_machines_completed ())
                    all_machines_completed = false;
            }

            if (all_machines_completed) {
                debug_msg ("AFU: machine completed");

                reset_machine_controllers ();

                if (psl_afu_aux2_change
                        (&afu_event, 0, 1, afu_event.job_cack_llcmd,
                         afu_event.job_error, afu_event.job_yield,
                         afu_event.timebase_request, afu_event.parity_enable,
                         afu_event.buffer_read_latency) != PSL_SUCCESS) {
                    error_msg ("AFU: asserting done failed");
                }
                state = IDLE;
            }
        }
    }
}

AFU::~AFU ()
{
    // close socket connection
    psl_close_afu_event (&afu_event);

    for (std::map < uint16_t, MachineController * >::iterator it =
                context_to_mc.begin (); it != context_to_mc.end (); ++it)
        delete it->second;

    context_to_mc.clear ();
}

void
AFU::resolve_aux1_event ()
{
    if (state == RUNNING)
        error_msg ("AFU: changing \"room\" when AFU is running");

    TagManager::set_max_credits (afu_event.room);
}

void
AFU::reset ()
{
    for (uint32_t i = 0; i < 3; ++i)
        global_configs[i] = 0;

    reset_delay = 0;

    reset_machine_controllers ();
}

void
AFU::reset_machine_controllers ()
{
    TagManager::reset ();

    for (std::map < uint16_t, MachineController * >::iterator it =
                context_to_mc.begin (); it != context_to_mc.end (); ++it)
        delete it->second;

    context_to_mc.clear ();

    if (descriptor.is_dedicated ()) {
        context_to_mc[0] = new MachineController (0);
        machine_controller = context_to_mc[0];
        highest_priority_mc = context_to_mc.end ();
    }


}

void
AFU::resolve_control_event ()
{
    // Check for job code parity
    if (afu_event.parity_enable)
        if (afu_event.job_code_parity !=
                generate_parity (afu_event.job_code, ODD_PARITY))
            error_msg ("AFU: Parity error in job_address");

    if (afu_event.job_code == PSL_JOB_RESET) {
        debug_msg ("AFU: received RESET");
        if (psl_afu_aux2_change
                (&afu_event, 0, afu_event.job_done, afu_event.job_cack_llcmd,
                 afu_event.job_error, afu_event.job_yield,
                 afu_event.timebase_request, afu_event.parity_enable,
                 afu_event.buffer_read_latency) != PSL_SUCCESS) {
            error_msg ("AFU: failed to de-assert job_running");
        }
        for (std::map < uint16_t, MachineController * >::iterator it =
                    context_to_mc.begin (); it != context_to_mc.end (); ++it)
            it->second->disable_all_machines ();
        state = RESET;
        reset_delay = 1000;
    }
    else if (afu_event.job_code == PSL_JOB_START) {
        debug_msg ("AFU: start signal recieved in state %d", state);
        if (state != READY)
            error_msg ("AFU: start signal detected outside of READY state");
        global_configs[1] = afu_event.job_address;

        // Check for jea parity
        if (afu_event.parity_enable && (afu_event.job_address_parity !=
                                        generate_parity (afu_event.job_address, ODD_PARITY))) {
            error_msg ("AFU: Parity error in job_address");
        }

        // assert job_running
        if (psl_afu_aux2_change
                (&afu_event, 1, afu_event.job_done, afu_event.job_cack_llcmd,
                 afu_event.job_error, afu_event.job_yield,
                 afu_event.timebase_request, afu_event.parity_enable,
                 afu_event.buffer_read_latency) != PSL_SUCCESS) {
            error_msg ("AFU: failed to assert job_running");
        }

        state = RUNNING;
        debug_msg ("AFU: AFU RUNNING");
    }
    // command for directed mode
    else if (afu_event.job_code == PSL_JOB_LLCMD) {
        // TODO test LLCMD
        switch (afu_event.job_address & PSL_LLCMD_MASK) {
        case PSL_LLCMD_ADD:
            if (context_to_mc.find (afu_event.job_address & 0xFFFF) !=
                    context_to_mc.end ()) {
                error_msg ("AFU: adding existing context %d",
                           afu_event.job_address & 0xFFFF);
            }
            context_to_mc[afu_event.job_address & 0xFFFF] =
                new MachineController (afu_event.job_address & 0xFFFF);
            if ((afu_event.job_address & 0xFFFF) == 0) {
                machine_controller = context_to_mc[0];
                highest_priority_mc = context_to_mc.end ();
            }
            break;
        case PSL_LLCMD_TERMINATE:
            if (context_to_mc.find (afu_event.job_address & 0xFFFF) ==
                    context_to_mc.end ()) {
                error_msg ("AFU: terminating non-existing context %d",
                           afu_event.job_address & 0xFFFF);
            }
            context_to_mc[afu_event.
                          job_address & 0xFFFF]->disable_all_machines ();
            break;
        case PSL_LLCMD_REMOVE:
            //TODO also make sure ADD->TERMINATE->REMOVE
            if (context_to_mc.find (afu_event.job_address & 0xFFFF) ==
                    context_to_mc.end ()) {
                error_msg ("AFU: removing non-existing context %d",
                           afu_event.job_address & 0xFFFF);
            }
            if (!context_to_mc
                    [afu_event.job_address & 0xFFFF]->all_machines_completed ())
            {
                error_msg
                ("AFU: removing context %d when command(s) still pending",
                 afu_event.job_address & 0xFFFF);
            }
            delete context_to_mc[afu_event.job_address & 0xFFFF];

            context_to_mc.erase (afu_event.job_address & 0xFFFF);
            break;
        default:
            error_msg ("AFU: this LLCMD code is currently not supported");
        }
    }
}

void
AFU::resolve_mmio_descriptor_event ()
{
    if (afu_event.mmio_read) {
        uint64_t data = descriptor.get_reg (afu_event.mmio_address,
                                            afu_event.mmio_double);
        uint32_t parity =
            (get_mmio_read_parity ())? generate_parity (data + 1,
                    1) :
            generate_parity (data, 1);

        if (psl_afu_mmio_ack (&afu_event, data, parity) != PSL_SUCCESS)
            error_msg ("AFU: mmio_ack failed");
    }
    else {
        error_msg ("AFU: descriptor write is not supported");
    }
}

void
AFU::resolve_mmio_event ()
{

    // MMIO READ
    if (afu_event.mmio_read) {
        uint64_t data = 0;

        // for global config
        if (afu_event.mmio_address < GLOBAL_CONFIG_OFFSET) {
            switch (afu_event.mmio_address & ~0x1) {
            case 0x4:
                data = global_configs[2];
                break;
            case 0x2:
                data = global_configs[1];
                break;
            default:
                data = 0xFFFFFFFFFFFFFFFFLL;
            }

            if (!afu_event.mmio_double) {
                if (afu_event.mmio_address & 0x1)
                    data = (data & 0xFFFFFFFF) | ((data & 0xFFFFFFFF) << 32);
                else
                    data = (data & 0xFFFFFFFF00000000LL) | (data >> 32);
            }
        }
        // part of machine controller/context
        else {
            MachineController *mc = NULL;

            if (context_to_mc.find ((afu_event.mmio_address -
                                     GLOBAL_CONFIG_OFFSET) / CONTEXT_SIZE) !=
                    context_to_mc.end ())
                mc = context_to_mc[(afu_event.mmio_address -
                                    GLOBAL_CONFIG_OFFSET) / CONTEXT_SIZE];

            if (mc != NULL) {
                data =
                    mc->
                    get_machine_config (afu_event.mmio_address & CONTEXT_MASK,
                                        afu_event.mmio_double);
            }
            else {
                data = 0xFFFFFFFFFFFFFFFFLL;
            }
        }

        debug_msg
        ("AFU: read mmio data address 0x%x, data 0x%016lx, double word %d",
         afu_event.mmio_address, data, afu_event.mmio_double);
        uint32_t parity =
            (get_mmio_read_parity ())? generate_parity (data + 1,
                    1) :
            generate_parity (data, 1);
        if (psl_afu_mmio_ack (&afu_event, data, parity) != PSL_SUCCESS)
            error_msg ("AFU: mmio_ack failed");
    }
    // MMIO WRITE
    else {
        if (afu_event.mmio_double == 0
                && ((afu_event.mmio_wdata & 0xFFFFFFFF) !=
                    ((afu_event.mmio_wdata & 0xFFFFFFFF00000000LL) >> 32))) {
            error_msg ("AFU: mmio double write data not duplicated");
        }
        debug_msg
        ("AFU: write mmio data address 0x%x, data 0x%016lx, double word %d",
         afu_event.mmio_address, afu_event.mmio_wdata,
         afu_event.mmio_double);

        if (afu_event.mmio_address < GLOBAL_CONFIG_OFFSET) {
            switch (afu_event.mmio_address & ~0x1) {
                // shut down afu
            case 0x00:
                for (std::map < uint16_t, MachineController * >::iterator it =
                            context_to_mc.begin (); it != context_to_mc.end ();
                        ++it) {
                    if (it->second->is_enabled ())
                        error_msg
                        ("AFU: attempt to turn off AFU when one or more machines are still enabled");
                }

                state = WAITING_FOR_LAST_RESPONSES;
                debug_msg ("AFU: preparing to shut down machines");
                break;
            case 0x04:
                global_configs[2] =
                    afu_event.mmio_wdata & 0x8000000000000000LL;
                break;
            default:
                warn_msg
                ("AFU: mmio write to invalid address, data dropped");
                break;
            }
        }
        else {
            MachineController *mc = NULL;

            if (context_to_mc.find ((afu_event.mmio_address -
                                     GLOBAL_CONFIG_OFFSET) / CONTEXT_SIZE) !=
                    context_to_mc.end ())
                mc = context_to_mc[(afu_event.mmio_address -
                                    GLOBAL_CONFIG_OFFSET) / CONTEXT_SIZE];

            if (mc != NULL)
                mc->change_machine_config (afu_event.mmio_address &
                                           CONTEXT_MASK, afu_event.mmio_wdata,
                                           afu_event.mmio_double);
            else {
                warn_msg
                ("AFU: mmio write to invalid address, data dropped");
            }
        }

        if (psl_afu_mmio_ack (&afu_event, 0, 0) != PSL_SUCCESS)
            error_msg ("AFU: mmio_ack failed");
    }

}

void
AFU::resolve_response_event (uint32_t cycle)
{
    if (!TagManager::is_in_use (afu_event.response_tag))
        error_msg ("AFU: received tag not in use");


    for (std::map < uint16_t, MachineController * >::iterator it =
                context_to_mc.begin (); it != context_to_mc.end (); ++it) {
        if (it->second->has_tag (afu_event.response_tag)) {
            it->second->process_response (&afu_event, cycle);
            break;
        }
    }
}

void
AFU::resolve_buffer_write_event ()
{
    if (!TagManager::is_in_use (afu_event.buffer_write_tag))
        error_msg ("AFU: received tag not in use");

    for (std::map < uint16_t, MachineController * >::iterator it =
                context_to_mc.begin (); it != context_to_mc.end (); ++it) {
        if (it->second->has_tag (afu_event.buffer_write_tag)) {
            it->second->process_buffer_write (&afu_event);
            break;
        }
    }
}

void
AFU::resolve_buffer_read_event ()
{
    if (!TagManager::is_in_use (afu_event.buffer_read_tag))
        error_msg ("AFU: received tag not in use");

    for (std::map < uint16_t, MachineController * >::iterator it =
                context_to_mc.begin (); it != context_to_mc.end (); ++it) {
        if (it->second->has_tag (afu_event.buffer_read_tag)) {
            it->second->process_buffer_read (&afu_event);
            break;
        }
    }
}

void
AFU::set_seed ()
{
    srand (time (NULL));
}

void
AFU::set_seed (uint32_t seed)
{
    srand (seed);
}

bool AFU::get_mmio_read_parity ()
{
    return (global_configs[2] & 0x8000000000000000LL) == 0x8000000000000000;
}
