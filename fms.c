/*
 *      fms.c -- fms decoder and packet dump
 *
 *      Copyright (C) 2007
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* ---------------------------------------------------------------------- */

#include "multimon.h"
#include <string.h>

/* ---------------------------------------------------------------------- */

static inline int fms_check_crc(uint64_t message)
{
    // TODO
}

/* ---------------------------------------------------------------------- */

static void fms_disp_service_id(uint8_t service_id)
{
    verbprintf(0, "%1x=", service_id);
    switch (service_id)
    {
        case 0x0: verbprintf(0, "UNKNOWN  \t"); break;
        case 0x1: verbprintf(0, "ASB      \t"); break;
        case 0x2: verbprintf(0, "KatS     \t"); break;
        case 0x3: verbprintf(0, "DLRG     \t"); break;
        case 0x4: verbprintf(0, "BGS      \t"); break;
        case 0x5: verbprintf(0, "JOHANNIER\t"); break;
        case 0x6: verbprintf(0, "FeuerWehr\t"); break;
        case 0x7: verbprintf(0, "ZIVILSCH \t"); break;
        case 0x8: verbprintf(0, "POLIZEI  \t"); break;
        case 0x9: verbprintf(0, "ROTKREUZ \t"); break;
        case 0xa: verbprintf(0, "ZOLL     \t"); break;
        case 0xb: verbprintf(0, "RETTUNGSD\t"); break;
        case 0xc: verbprintf(0, "BKA      \t"); break;
        case 0xd: verbprintf(0, "MALTESER \t"); break;
        case 0xe: verbprintf(0, "THW      \t"); break;
        case 0xf: verbprintf(0, "FernWirk \t"); break;
    }
}

static void fms_disp_state_id(uint8_t state_id, uint8_t loc_id)
{
    verbprintf(0, "%1x=", state_id);
    switch (state_id)
    {
        case 0x0: verbprintf(0, "Sachsen  \t"); break;
        case 0x1: verbprintf(0, "NiederSac\t"); break;
        case 0x2: verbprintf(0, "Berlin   \t"); break;
        case 0x3: verbprintf(0, "Saarland \t"); break;
        case 0x4: verbprintf(0, "Baden-Wür\t"); break;
        case 0x5: verbprintf(0, "Rheinl-Pf\t"); break;
        case 0x6: verbprintf(0, "Hamburg  \t"); break;
        case 0x7: if (loc_id < 50) verbprintf(0, "Meck-Vorp\t"); else verbprintf(0, "Sach-Anha\t"); break;
        case 0x8: verbprintf(0, "Bund     \t"); break;
        case 0x9: verbprintf(0, "Nordr-Wes\t"); break;
        case 0xa: verbprintf(0, "Bremen   \t"); break;
        case 0xb: verbprintf(0, "Bayern 2 \t"); break;
        case 0xc: verbprintf(0, "Bayern 1 \t"); break;
        case 0xd: verbprintf(0, "Schles-Ho\t"); break;
        case 0xe: verbprintf(0, "Hessen   \t"); break;
        case 0xf: if (loc_id < 50) verbprintf(0, "Brandenbu\t"); else verbprintf(0, "Thüringen\t"); break;
    }
}

static void fms_disp_loc_id(uint8_t loc_id)
{
    verbprintf(0, "%03d\t", loc_id);
}

static void fms_disp_vehicle_id_decimal(uint8_t nibble)
{
    switch (nibble)
    {
        case 0x0: verbprintf(0, "0"); break;
        case 0x1: verbprintf(0, "8"); break;
        case 0x2: verbprintf(0, "4"); break;
        case 0x3: verbprintf(0, "C"); break;
        case 0x4: verbprintf(0, "2"); break;
        case 0x5: verbprintf(0, "A"); break;
        case 0x6: verbprintf(0, "6"); break;
        case 0x7: verbprintf(0, "E"); break;
        case 0x8: verbprintf(0, "1"); break;
        case 0x9: verbprintf(0, "9"); break;
        case 0xa: verbprintf(0, "5"); break;
        case 0xb: verbprintf(0, "D"); break;
        case 0xc: verbprintf(0, "3"); break;
        case 0xd: verbprintf(0, "B"); break;
        case 0xe: verbprintf(0, "7"); break;
        case 0xf: verbprintf(0, "F"); break;
    }
}

static void fms_disp_vehicle_id(uint16_t vehicle_id)
{
    verbprintf(0, "%04x=", vehicle_id, vehicle_id);

    // Maybe we need to switch/reverse nibbles as well?
    uint8_t nib0 = (vehicle_id >> 12) & 0xF;
    uint8_t nib1 = (vehicle_id >> 8) & 0xF;
    uint8_t nib2 = (vehicle_id >> 4) & 0xF;
    uint8_t nib3 = (vehicle_id) & 0xF;

    fms_disp_vehicle_id_decimal(nib0);
    fms_disp_vehicle_id_decimal(nib1);
    fms_disp_vehicle_id_decimal(nib2);
    fms_disp_vehicle_id_decimal(nib3);

    verbprintf(0, "\t");
}

static void fms_disp_state(uint8_t state, uint8_t service_id, uint8_t direction)
{
    // TODO: Other services?
    if (direction == 0){
        // FZG -> LST
        switch (state)
        {
            case 0x0: verbprintf(0, "EMERGENCY\t"); break;
            case 0x1: verbprintf(0, "Am TZiel \t"); break;
            case 0x2: verbprintf(0, "Am EZiel \t"); break;
            case 0x3: verbprintf(0, "Sonder 1 \t"); break;
            case 0x4: verbprintf(0, "Brt Wache\t"); break;
            case 0x5: verbprintf(0, "Vrbrt Flg\t"); break; // Vorbereitung Folgetelegram
            case 0x6: verbprintf(0, "Nicht Brt\t"); break;
            case 0x7: verbprintf(0, "AutomQuit\t"); break; // Automatische Quittung
            case 0x8: verbprintf(0, "Einbuchen\t"); break;
            case 0x9: verbprintf(0, "Arzt Aufg\t"); break; // Arzt aufgenommen / Handquittung / Anmeldung im Fremdkreis / Dringender Sprechwunsch
            case 0xa: verbprintf(0, "Sprechwun\t"); break;
            case 0xb: verbprintf(0, "Sonder 2 \t"); break;
            case 0xc: verbprintf(0, "Estz AB  \t"); break; // Einsatz übernommen, "Ab"
            case 0xd: verbprintf(0, "Bendg Flg\t"); break; // Beendigung Folgetelegram
            case 0xe: verbprintf(0, "Pat aufge\t"); break; // Patient aufgenommen
            case 0xf: verbprintf(0, "Sprechtas\t"); break;
        }
    }
    else
    {
        // LST -> FZG
        switch (state)
        {
            case 0x0: verbprintf(0, "StatusAbf\t"); break;
            case 0x1: verbprintf(0, "FernWrk 1\t"); break;
            case 0x2: verbprintf(0, "Spezifi 4\t"); break;
            case 0x3: verbprintf(0, "KurzTXT C\t"); break;
            case 0x4: verbprintf(0, "Spezifi 2\t"); break;
            case 0x5: verbprintf(0, "Vrbrt TXT\t"); break;
            case 0x6: verbprintf(0, "Spezifi 6\t"); break;
            case 0x7: verbprintf(0, "KurzTXT E\t"); break;
            case 0x8: verbprintf(0, "SammelRuf\t"); break;
            case 0x9: verbprintf(0, "FernWrk 2\t"); break;
            case 0xa: verbprintf(0, "Sprechwun\t"); break;
            case 0xb: verbprintf(0, "Spezifi 5\t"); break;
            case 0xc: verbprintf(0, "Spezifi 3\t"); break;
            case 0xd: verbprintf(0, "Bendg TXT\t"); break;
            case 0xe: verbprintf(0, "Spezifi 7\t"); break;
            case 0xf: verbprintf(0, "AutomQuit\t"); break; // Automatische Quittung
        }
    }
}

static void fms_disp_direction(uint8_t direction)
{
    if (direction)
    {
        verbprintf(0, "1=LST->FZG\t");
    }
    else
    {
        verbprintf(0, "0=FZG->LST\t");
    }
}

/*
 *  As specified in http://www.lfs-bw.de/Fachthemen/Digitalfunk-Funk/Documents/Pruefstelle/TRBOS-FMS.pdf
 */
static void fms_disp_packet(uint64_t message)
{
    uint8_t service_id;  // BOS-Kennung
    uint8_t state_id;    // Landeskennung
    uint8_t loc_id;      // Ortskennung
    uint16_t vehicle_id; // Fahrzeugkennung
    uint8_t state;       // Status
    uint8_t model;       // Baustufenkennung
    uint8_t direction;   // Richtungskennung
    uint8_t short_info;  // taktische Kurzinformation
    uint8_t crc;         // Redundanz

    verbprintf(0, "FMS MESSAGE HIGH: %08x\n", message >> 32);
    verbprintf(0, "FMS MESSAGE  LOW: %08x\n", message);

    service_id = (message >> 44) & 0xF;
    fms_disp_service_id(service_id);

    state_id = (message >> 40) & 0xF;
    loc_id = (message >> 32) & 0xFF;
    fms_disp_state_id(state_id, loc_id);
    fms_disp_loc_id(loc_id);

    vehicle_id = (message >> 16) & 0xFFFF;
    fms_disp_vehicle_id(vehicle_id);

    state = (message >> 12) & 0xF;

    model = (message >> 11) & 0x1;
    direction = (message >> 10) & 0x1;
    fms_disp_state(state, service_id, direction);

    fms_disp_direction(direction);

    short_info = (message >> 8) & 0x3;

    crc = (message >> 2) & 0x3F;

    verbprintf(0, "\n\n");
}

/* ---------------------------------------------------------------------- */

void fms_init(struct demod_state *s)
{
    memset(&s->l2.uart, 0, sizeof(s->l2.uart));
    s->l2.fmsfsk.rxstate = 0;
    s->l2.fmsfsk.rxbitstream = 0;
    s->l2.fmsfsk.rxbitcount = 0;
}

/* ---------------------------------------------------------------------- */

void fms_rxbit(struct demod_state *s, int bit)
{
  // Append the bit to our "state machine tracker" so we can properly detect sync bits
  s->l2.fmsfsk.rxstate = ((s->l2.fmsfsk.rxstate << 1) & 0x000FFFFE) | bit;

  // Check if the sync pattern is in the buffer
  if ((s->l2.fmsfsk.rxstate & 0x0007FFFF) == 0x7FF1A)
  {
       verbprintf(0, "FMS ->SYNC<-\n");
       s->l2.fmsfsk.rxbitstream = 0; // reset RX buffer
       s->l2.fmsfsk.rxbitcount = 1;  // > 1 means we have a valid SYNC
  }

  // If we have a valid SYNC, record the message by appending it to the RX buffer
  else if (s->l2.fmsfsk.rxbitcount >= 1) {
    s->l2.fmsfsk.rxbitstream = (s->l2.fmsfsk.rxbitstream << 1) | bit;
    s->l2.fmsfsk.rxbitcount++;

    // Wait until message has been completely received
    if (s->l2.fmsfsk.rxbitcount == 49)
    {
      fms_disp_packet(s->l2.fmsfsk.rxbitstream);
      s->l2.fmsfsk.rxbitcount = 0; // Reset counter, meaning "no valid SYNC yet"
      s->l2.fmsfsk.rxstate = 0;    // Reset message input buffer
    }
  }
}

/* ---------------------------------------------------------------------- */
