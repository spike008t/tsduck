//----------------------------------------------------------------------------
//
// TSDuck - The MPEG Transport Stream Toolkit
// Copyright (c) 2005-2017, Thierry Lelegard
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//
//----------------------------------------------------------------------------
//
//  Transport stream processor shared library:
//  Remove a service
//
//----------------------------------------------------------------------------

#include "tsPlugin.h"
#include "tsService.h"
#include "tsSectionDemux.h"
#include "tsCyclingPacketizer.h"
#include "tsNames.h"
#include "tsFormat.h"
#include "tsTables.h"



//----------------------------------------------------------------------------
// Plugin definition
//----------------------------------------------------------------------------

namespace ts {
    class SVRemovePlugin: public ProcessorPlugin, private TableHandlerInterface
    {
    public:
        // Implementation of plugin API
        SVRemovePlugin (TSP*);
        virtual bool start();
        virtual bool stop() {return true;}
        virtual BitRate getBitrate() {return 0;}
        virtual Status processPacket (TSPacket&, bool&, bool&);

    private:
        bool              _abort;          // Error (service not found, etc)
        bool              _ready;          // Ready to pass packets
        bool              _transparent;    // Transparent mode, pass all packets
        Service           _service;        // Service name & id
        bool              _ignore_absent;  // Ignore service if absent
        bool              _ignore_bat;     // Do not modify the BAT
        bool              _ignore_nit;     // Do not modify the NIT
        Status            _drop_status;    // Status for dropped packets
        PIDSet            _drop_pids;      // List of PIDs to drop
        PIDSet            _ref_pids;       // List of other referenced PIDs
        SectionDemux      _demux;          // Section demux
        CyclingPacketizer _pzer_pat;       // Packetizer for modified PAT
        CyclingPacketizer _pzer_sdt_bat;   // Packetizer for modified SDT/BAT
        CyclingPacketizer _pzer_nit;       // Packetizer for modified NIT

        // Invoked by the demux when a complete table is available.
        virtual void handleTable (SectionDemux&, const BinaryTable&);

        // Process specific tables and descriptors
        void processPAT (PAT&);
        void processSDT (SDT&);
        void processPMT (PMT&);
        void processNITBAT (AbstractTransportListTable&);
        void processNITBATDescriptorList (DescriptorList&);

        // Mark all ECM PIDs from the specified descriptor list in the specified PID set
        void addECMPID (const DescriptorList&, PIDSet&);
    };
}

TSPLUGIN_DECLARE_VERSION
TSPLUGIN_DECLARE_PROCESSOR(ts::SVRemovePlugin)


//----------------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------------

ts::SVRemovePlugin::SVRemovePlugin (TSP* tsp_) :
    ProcessorPlugin (tsp_, "Remove a service.", "[options] service"),
    _demux (this),
    _pzer_pat (PID_PAT, CyclingPacketizer::ALWAYS),
    _pzer_sdt_bat (PID_SDT, CyclingPacketizer::ALWAYS),
    _pzer_nit (PID_NIT, CyclingPacketizer::ALWAYS)
{
    option ("",               0,  STRING, 1, 1);
    option ("ignore-absent", 'a');
    option ("ignore-bat",    'b');
    option ("ignore-nit",    'n');
    option ("stuffing",      's');

    setHelp ("Service:\n"
             "  Specifies the service to remove. If the argument is an integer value\n"
             "  (either decimal or hexadecimal), it is interpreted as a service id.\n"
             "  Otherwise, it is interpreted as a service name, as specified in the SDT.\n"
             "  The name is not case sensitive and blanks are ignored.\n"
             "\n"
             "Options:\n"
             "\n"
             "  --help\n"
             "      Display this help text.\n"
             "\n"
             "  -a\n"
             "  --ignore-absent\n"
             "      Ignore service if not present in the transport stream. By default, tsp\n"
             "      fails if the service is not found.\n"
             "\n"
             "  -b\n"
             "  --ignore-bat\n"
             "      Do not modify the BAT.\n"
             "\n"
             "  -n\n"
             "  --ignore-nit\n"
             "      Do not modify the NIT.\n"
             "\n"
             "  -s\n"
             "  --stuffing\n"
             "      Replace excluded packets with stuffing (null packets) instead\n"
             "      of removing them. Useful to preserve bitrate.\n"
             "\n"
             "  --version\n"
             "      Display the version number.\n");
}


//----------------------------------------------------------------------------
// Start method
//----------------------------------------------------------------------------

bool ts::SVRemovePlugin::start()
{
    // Get option values
    _service.set (value (""));
    _ignore_absent = present ("ignore-absent");
    _ignore_bat = present ("ignore-bat");
    _ignore_nit = present ("ignore-nit");
    _drop_status = present ("stuffing") ? TSP_NULL : TSP_DROP;

    // Initialize the demux
    _demux.reset();
    _demux.addPID (PID_SDT);

    // When the service id is known, we wait for the PAT. If it is not yet
    // known (only the service name is known), we do not know how to modify
    // the PAT. We will wait for it after receiving the SDT.
    // Packets from PAT PID are analyzed but not passed. When a complete
    // PAT is read, a modified PAT will be transmitted.
    if (_service.hasId()) {
        _demux.addPID (PID_PAT);
        if (!_ignore_nit) {
            _demux.addPID (PID_NIT);
        }
    }

    // Build a list of referenced PID's (except those in the removed service).
    // Prevent predefined PID's from being removed.
    _ref_pids.reset();
    _ref_pids.set (PID_PAT);
    _ref_pids.set (PID_CAT);
    _ref_pids.set (PID_TSDT);
    _ref_pids.set (PID_NULL);  // keep stuffing as well
    _ref_pids.set (PID_NIT);
    _ref_pids.set (PID_SDT);   // also contains BAT
    _ref_pids.set (PID_EIT);
    _ref_pids.set (PID_RST);
    _ref_pids.set (PID_TDT);   // also contains TOT
    _ref_pids.set (PID_NETSYNC);
    _ref_pids.set (PID_RNT);
    _ref_pids.set (PID_INBSIGN);
    _ref_pids.set (PID_MEASURE);
    _ref_pids.set (PID_DIT);
    _ref_pids.set (PID_SIT);

    // Reset other states
    _abort = false;
    _ready = false;
    _transparent = false;
    _drop_pids.reset();
    _pzer_pat.reset();
    _pzer_sdt_bat.reset();
    _pzer_nit.reset();

    return true;
}


//----------------------------------------------------------------------------
// Invoked by the demux when a complete table is available.
//----------------------------------------------------------------------------

void ts::SVRemovePlugin::handleTable (SectionDemux& demux, const BinaryTable& table)
{
    if (tsp->debug()) {
        std::string name (names::TID (table.tableId()));
        tsp->debug ("Got %s v%d, PID %d (0x%04X), TIDext %d (0x%04X)",
                    name.c_str(), int (table.version()),
                    int (table.sourcePID()), int (table.sourcePID()),
                    int (table.tableIdExtension()), int (table.tableIdExtension()));
    }

    switch (table.tableId()) {

        case TID_PAT: {
            if (table.sourcePID() == PID_PAT) {
                PAT pat (table);
                if (pat.isValid()) {
                    processPAT (pat);
                }
            }
            break;
        }

        case TID_PMT: {
            PMT pmt (table);
            if (pmt.isValid()) {
                processPMT (pmt);
            }
            break;
        }

        case TID_SDT_ACT: {
            if (table.sourcePID() == PID_SDT) {
                SDT sdt (table);
                if (sdt.isValid()) {
                    processSDT (sdt);
                }
            }
            break;
        }

        case TID_SDT_OTH: {
            if (table.sourcePID() == PID_SDT) {
                // SDT Other are passed unmodified
                _pzer_sdt_bat.removeSections (TID_SDT_OTH, table.tableIdExtension());
                _pzer_sdt_bat.addTable (table);
            }
            break;
        }

        case TID_BAT:
            if (table.sourcePID() == PID_BAT) {
                if (!_service.hasId()) {
                    // The BAT and SDT are on the same PID. Here, we are in the case
                    // were the service was designated by name and the first BAT arrives
                    // before the first SDT. We do not know yet how to modify the BAT.
                    // Reset the demux on this PID, so that this BAT will be submitted
                    // again the next time.
                    _demux.resetPID (table.sourcePID());
                }
                else if (_ignore_bat) {
                    // Do not modify BAT
                    _pzer_sdt_bat.removeSections (TID_BAT, table.tableIdExtension());
                    _pzer_sdt_bat.addTable (table);
                }
                else {
                    // Modify BAT
                    BAT bat (table);
                    if (bat.isValid()) {
                        processNITBAT (bat);
                        _pzer_sdt_bat.removeSections (TID_BAT, bat.bouquet_id);
                        _pzer_sdt_bat.addTable (bat);
                    }
                }
            }
            break;

        case TID_NIT_ACT: {
            if (table.sourcePID() == PID_NIT) {
                if (_ignore_nit) {
                    // Do not modify NIT Actual
                    _pzer_nit.removeSections (TID_NIT_ACT, table.tableIdExtension());
                    _pzer_nit.addTable (table);
                }
                else {
                    // Modify NIT Actual
                    NIT nit (table);
                    if (nit.isValid()) {
                        processNITBAT (nit);
                        _pzer_nit.removeSections (TID_NIT_ACT, nit.network_id);
                        _pzer_nit.addTable (nit);
                    }
                }
            }
            break;
        }

        case TID_NIT_OTH: {
            if (table.sourcePID() == PID_NIT) {
                // NIT Other are passed unmodified
                _pzer_nit.removeSections (TID_NIT_OTH, table.tableIdExtension());
                _pzer_nit.addTable (table);
            }
            break;
        }
    }
}


//----------------------------------------------------------------------------
//  This method processes a Service Description Table (SDT).
//----------------------------------------------------------------------------

void ts::SVRemovePlugin::processSDT (SDT& sdt)
{
    bool found = false;

    // Look for the service by name or by id
    if (_service.hasId()) {
        // Search service by id
        found = sdt.services.find (_service.getId()) != sdt.services.end();
        if (!found) {
            // Informational only, SDT entry is not mandatory.
            tsp->info ("service %d (0x%04X) not found in SDT, ignoring it", int (_service.getId()), int (_service.getId()));
        }
    }
    else {
        // Service id is currently unknown, search service by name
        found = sdt.findService (_service);
        if (!found) {
            // Here, this is an error. A service can be searched by name only in current TS
            if (_ignore_absent) {
                tsp->warning ("service \"" + _service.getName() + "\" not found in SDT, ignoring it");
                _transparent = true;
            }
            else {
                tsp->error ("service \"" + _service.getName() + "\" not found in SDT");
                _abort = true;
            }
            return;
        }
        // The service id was previously unknown, now wait for the PAT
        _demux.addPID (PID_PAT);
        if (!_ignore_nit) {
            _demux.addPID (PID_NIT);
        }
        tsp->verbose ("found service \"" + _service.getName() + Format ("\", service id is 0x%04X", int (_service.getId())));
    }

    // Remove service description in the SDT
    if (_service.hasId()) {
        sdt.services.erase (_service.getId());
    }

    // Replace the SDT in the PID
    _pzer_sdt_bat.removeSections (TID_SDT_ACT, sdt.ts_id);
    _pzer_sdt_bat.addTable (sdt);
}


//----------------------------------------------------------------------------
//  This method processes a Program Association Table (PAT).
//----------------------------------------------------------------------------

void ts::SVRemovePlugin::processPAT (PAT& pat)
{
    // PAT not normally fetched until service id is known
    assert (_service.hasId());

    // Save the NIT PID
    _pzer_nit.setPID (pat.nit_pid);
    _demux.addPID (pat.nit_pid);

    // Loop on all services in the PAT. We need to scan all PMT's to know which
    // PID to remove and which to keep (if shared between the removed service
    // and other services).
    bool found = false;
    for (PAT::ServiceMap::const_iterator it = pat.pmts.begin(); it != pat.pmts.end(); ++it) {
        // Scan all PMT's
        _demux.addPID (it->second);

        // Check if service to remove is here
        if (it->first == _service.getId()) {
            found = true;
            _service.setPMTPID (it->second);
            tsp->verbose ("found service id 0x%04X, PMT PID is 0x%04X", int (_service.getId()), int (_service.getPMTPID()));
            // Drop PMT of the service
            _drop_pids.set (it->second);
        }
        else {
            // Mark other PMT's as referenced
            _ref_pids.set (it->second);
        }
    }

    if (found) {
        // Remove the service from the PAT
        pat.pmts.erase (_service.getId());
    }
    else if (_ignore_absent || !_ignore_nit || !_ignore_bat) {
        // Service is not present in current TS, but continue
        tsp->info ("service id 0x%04X not found in PAT, ignoring it", int (_service.getId()));
        _ready = true;
    }
    else {
        // If service is not found and no need to modify to NIT or BAT, abort
        tsp->error ("service id 0x%04X not found in PAT", int (_service.getId()));
        _abort = true;
    }

    // Replace the PAT.in the PID
    _pzer_pat.removeSections (TID_PAT);
    _pzer_pat.addTable (pat);
}


//----------------------------------------------------------------------------
//  This method processes a Program Map Table (PMT).
//----------------------------------------------------------------------------

void ts::SVRemovePlugin::processPMT (PMT& pmt)
{
    // Is this the PMT of the service to remove?
    const bool removed_service = pmt.service_id == _service.getId();

    // Mark PIDs as dropped or referenced.
    PIDSet& pid_set (removed_service ? _drop_pids : _ref_pids);

    // Mark all program-level ECM PID's
    addECMPID (pmt.descs, pid_set);

    // Mark service's PCR PID (usually a referenced component or null PID)
    pid_set.set (pmt.pcr_pid);

    // Loop on all elementary streams
    for (PMT::StreamMap::const_iterator it = pmt.streams.begin(); it != pmt.streams.end(); ++it) {
        // Mark component's PID
        pid_set.set (it->first);
        // Mark all component-level ECM PID's
        addECMPID (it->second.descs, pid_set);
    }

    // When the service to remove has been analyzed, we are ready to filter PIDs
    _ready = _ready || removed_service;
}


//----------------------------------------------------------------------------
// Mark all ECM PIDs from the descriptor list in the PID set
//----------------------------------------------------------------------------

void ts::SVRemovePlugin::addECMPID (const DescriptorList& dlist, PIDSet& pid_set)
{
    // Loop on all CA descriptors
    for (size_t index = dlist.search(DID_CA); index < dlist.count(); index = dlist.search(DID_CA, index + 1)) {
        CADescriptor ca(*dlist[index]);
        if (!ca.isValid()) {
            // Cannot deserialize a valid CA descriptor, ignore it
        }
        else {
            // Standard CAS, only one PID in CA descriptor
            pid_set.set(ca.ca_pid);
        }
    }
}


//----------------------------------------------------------------------------
//  This method processes a NIT or a BAT
//----------------------------------------------------------------------------

void ts::SVRemovePlugin::processNITBAT (AbstractTransportListTable& table)
{
    // Process the global descriptor list
    processNITBATDescriptorList (table.descs);

    // Process each TS descriptor list
    for (AbstractTransportListTable::TransportMap::iterator it = table.transports.begin(); it != table.transports.end(); ++it) {
        processNITBATDescriptorList (it->second);
    }
}


//----------------------------------------------------------------------------
//  This method processes a NIT or a BAT descriptor list
//----------------------------------------------------------------------------

void ts::SVRemovePlugin::processNITBATDescriptorList (DescriptorList& dlist)
{
    // Process all service_list_descriptors
    for (size_t i = dlist.search (DID_SERVICE_LIST); i < dlist.count(); i = dlist.search (DID_SERVICE_LIST, i + 1)) {

        uint8_t* base = dlist[i]->payload();
        size_t size = dlist[i]->payloadSize();
        uint8_t* data = base;
        uint8_t* new_data = base;

        while (size >= 3) {
            if (GetUInt16 (data) != _service.getId()) {
                // Not the removed service, keep this entry
                new_data[0] = data[0];
                new_data[1] = data[1];
                new_data[2] = data[2];
                new_data += 3;
            }
            data += 3;
            size -= 3;
        }
        dlist[i]->resizePayload (new_data - base);
    }

    // Process all logical_channel_number_descriptors
    for (size_t i = dlist.search (DID_LOGICAL_CHANNEL_NUM, 0, PDS_EICTA);
         i < dlist.count();
         i = dlist.search (DID_LOGICAL_CHANNEL_NUM, i + 1, PDS_EICTA)) {

        uint8_t* base = dlist[i]->payload();
        size_t size = dlist[i]->payloadSize();
        uint8_t* data = base;
        uint8_t* new_data = base;

        while (size >= 4) {
            if (GetUInt16 (data) != _service.getId()) {
                // Not the removed service, keep this entry
                new_data[0] = data[0];
                new_data[1] = data[1];
                new_data[2] = data[2];
                new_data[3] = data[3];
                new_data += 4;
            }
            data += 4;
            size -= 4;
        }
        dlist[i]->resizePayload (new_data - base);
    }
}


//----------------------------------------------------------------------------
// Packet processing method
//----------------------------------------------------------------------------

ts::ProcessorPlugin::Status ts::SVRemovePlugin::processPacket (TSPacket& pkt, bool& flush, bool& bitrate_changed)
{
    const PID pid = pkt.getPID();

    // Pass packets in transparent mode
    if (_transparent) {
        return TSP_OK;
    }

    // Filter interesting sections
    _demux.feedPacket (pkt);

    // If a fatal error occured during section analysis, give up.
    if (_abort) {
        return TSP_END;
    }

    // As long as the original service-id or PMT are unknown, drop or nullify packets
    if (!_ready) {
        return _drop_status;
    }

    // Packets from removed PIDs are either dropped or nullified
    if (_drop_pids[pid] && !_ref_pids[pid]) {
        return _drop_status;
    }

    // Replace packets using packetizers
    if (pid == _pzer_pat.getPID()) {
        _pzer_pat.getNextPacket (pkt);
    }
    else if (pid == _pzer_sdt_bat.getPID()) {
        _pzer_sdt_bat.getNextPacket (pkt);
    }
    else if (!_ignore_nit && pid == _pzer_nit.getPID()) {
        _pzer_nit.getNextPacket (pkt);
    }

    return TSP_OK;
}
