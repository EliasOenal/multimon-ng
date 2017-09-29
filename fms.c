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

static void fms_disp_service_id(uint8_t service_id)
{
    verbprintf(0, "%1x=", service_id);
    switch (service_id)
    {
        case 0x0: verbprintf(0, "UNKNOWN       \t"); break;
        case 0x1: verbprintf(0, "POLIZEI       \t"); break;
        case 0x2: verbprintf(0, "BGS           \t"); break;
        case 0x3: verbprintf(0, "BKA           \t"); break;
        case 0x4: verbprintf(0, "KatS          \t"); break;
        case 0x5: verbprintf(0, "ZOLL          \t"); break;
        case 0x6: verbprintf(0, "Feuerwehr     \t"); break;
        case 0x7: verbprintf(0, "THW           \t"); break;
        case 0x8: verbprintf(0, "ASB           \t"); break;
        case 0x9: verbprintf(0, "Rotkreuz      \t"); break;
        case 0xa: verbprintf(0, "Johanniter    \t"); break;
        case 0xb: verbprintf(0, "Malteser      \t"); break;
        case 0xc: verbprintf(0, "DLRG          \t"); break;
        case 0xd: verbprintf(0, "Rettungsdienst\t"); break;
        case 0xe: verbprintf(0, "ZivilSchutz   \t"); break;
        case 0xf: verbprintf(0, "FernWirk      \t"); break;
    }
}

static void fms_disp_state_id(uint8_t state_id, uint8_t loc_id)
{
    verbprintf(0, "%1x=", state_id);
    switch (state_id)
    {
        case 0x0: verbprintf(0, "Sachsen         \t"); break;
        case 0x1: verbprintf(0, "Bund            \t"); break;
        case 0x2: verbprintf(0, "Baden-Wurtemberg\t"); break;
        case 0x3: verbprintf(0, "Bayern 1        \t"); break;
        case 0x4: verbprintf(0, "Berlin          \t"); break;
        case 0x5: verbprintf(0, "Bremen          \t"); break;
        case 0x6: verbprintf(0, "Hamburg         \t"); break;
        case 0x7: verbprintf(0, "Hessen          \t"); break;
        case 0x8: verbprintf(0, "Niedersachsen   \t"); break;
        case 0x9: verbprintf(0, "Nordrhein-Wesfal\t"); break;
        case 0xa: verbprintf(0, "Rheinland-Pfalz \t"); break;
        case 0xb: verbprintf(0, "Schleswig-Holste\t"); break;
        case 0xc: verbprintf(0, "Saarland        \t"); break;
        case 0xd: verbprintf(0, "Bayern 2        \t"); break;
        case 0xe: if (loc_id < 50) verbprintf(0, "Meckl-Vorpommern\t"); else verbprintf(0, "Sachsen-Anhalt  \t"); break;
        case 0xf: if (loc_id < 50) verbprintf(0, "Brandenburg     \t"); else verbprintf(0, "Thuringen       \t"); break;
    }
}

static void fms_disp_loc_id(uint8_t loc_id)
{
    //fix due to wrong location id
    //now we are according to TR-BOS
    uint8_t tmp = 0;
    tmp = loc_id;
    loc_id <<= 4;
    tmp >>= 4;
    loc_id = loc_id^tmp;
    
    verbprintf(0, "Ort 0x%2x=%03d\t", loc_id, loc_id);
}

static void fms_disp_vehicle_id(uint16_t vehicle_id)
{
    uint8_t nib0 = (vehicle_id) & 0xF;
    uint8_t nib1 = (vehicle_id >> 4) & 0xF;
    uint8_t nib2 = (vehicle_id >> 8) & 0xF;
    uint8_t nib3 = (vehicle_id >> 12) & 0xF;

    verbprintf(0, "FZG %1x%1x%1x%1x\t", nib0, nib1, nib2, nib3);
}

static void fms_disp_state(uint8_t state, uint8_t direction)
{
    verbprintf(0, "Status %1x=", state);

    // TODO: Other services?
    if (direction == 0){
        // FZG -> LST
        switch (state)
        {
            case 0x0: verbprintf(0, "Notfall       \t"); break;
            case 0x1: verbprintf(0, "Einbuchen     \t"); break;
            case 0x2: verbprintf(0, "Bereit Wache  \t"); break;
            case 0x3: verbprintf(0, "Einsatz Ab    \t"); break; // Einsatz übernommen, "Ab"
            case 0x4: verbprintf(0, "Am EinsatzZiel\t"); break; // "AN"
            case 0x5: verbprintf(0, "Sprechwunsch  \t"); break;
            case 0x6: verbprintf(0, "Nicht Bereit  \t"); break;
            case 0x7: verbprintf(0, "Patient aufgen\t"); break; // Patient aufgenommen
            case 0x8: verbprintf(0, "Am TranspZiel \t"); break;
            case 0x9: verbprintf(0, "Arzt Aufgenomm\t"); break; // Arzt aufgenommen / Handquittung / Anmeldung im Fremdkreis / Dringender Sprechwunsch
            case 0xa: verbprintf(0, "Vorbertg Folge\t"); break; // Vorbereitung Folgetelegram
            case 0xb: verbprintf(0, "Beendig  Folge\t"); break; // Beendigung Folgetelegram
            case 0xc: verbprintf(0, "Sonder 1      \t"); break;
            case 0xd: verbprintf(0, "Sonder 2      \t"); break;
            case 0xe: verbprintf(0, "AutomatQuittun\t"); break; // Automatische Quittung
            case 0xf: verbprintf(0, "Sprechtaste   \t"); break;
        }
    }
    else
    {
        // LST -> FZG
        switch (state)
        {
            case 0x0: verbprintf(0, "StatusAbfrage \t"); break;
            case 0x1: verbprintf(0, "SammelRuf     \t"); break;
            case 0x2: verbprintf(0, "Einrucken/Abbr\t"); break; // Einrücken / Einsatz abgebrochen
            case 0x3: verbprintf(0, "Ubernahme     \t"); break; // Melden für Einsatzübernahme
            case 0x4: verbprintf(0, "Kommen Draht  \t"); break; // "Kommen Sie über Draht"
            case 0x5: verbprintf(0, "Fahre Wache   \t"); break; // "Fahren Sie Wache an"
            case 0x6: verbprintf(0, "Sprechaufford \t"); break; // Sprechaufforderung
            case 0x7: verbprintf(0, "Lagemeldung   \t"); break; // "Geben Sie Lagemeldung"
            case 0x8: verbprintf(0, "FernWirk 1    \t"); break;
            case 0x9: verbprintf(0, "FernWirk 2    \t"); break;
            case 0xa: verbprintf(0, "Vorbertg TXT  \t"); break;
            case 0xb: verbprintf(0, "Beendig  TXT  \t"); break;
            case 0xc: verbprintf(0, "KurzTXT C     \t"); break;
            case 0xd: verbprintf(0, "KurzTXT D     \t"); break;
            case 0xe: verbprintf(0, "KurzTXT E     \t"); break;
            case 0xf: verbprintf(0, "AutomatQuittun\t"); break; // Automatische Quittung
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

static void fms_disp_shortinfo(uint8_t short_info)
{
    verbprintf(0, "%1x=", short_info);

    switch (short_info)
    {
        case 0x0: verbprintf(0, "I  (ohneNA,ohneSIGNAL)\t"); break;
        case 0x1: verbprintf(0, "II (ohneNA,mit SIGNAL)\t"); break;
        case 0x2: verbprintf(0, "III(mit NA,ohneSIGNAL)\t"); break;
        case 0x3: verbprintf(0, "IV (mit NA,mit SIGNAL)\t"); break;
        }
}

static void fms_print_crc(char crc[7]) {
  int i;

  verbprintf(2, "FMS CRC:");

  for (i = 0; i < 7; ++i) {
    if (crc[i]) {
      verbprintf(2, "1");
    } else {
      verbprintf(2, "0");
    }
  }

  verbprintf(2, "\n");
}

void fms_print_message_hex(uint64_t message)
{
    verbprintf(2, "FMS MESSAGE HIGH: %08x\n", message >> 32);
    verbprintf(2, "FMS MESSAGE  LOW: %08x\n", message);
}


static char fms_is_crc_correct(uint64_t message)
{
    int i;
    char crc[7];
    char doinvert;

    // Initialize the crc to 0s
    for (i=0; i<7; ++i)
    {
        crc[i] = 0;
    }


    for (i=0; i<48; ++i)
    {
        // update the running CRC
        // The implementation is not the most performant one but it suffices for my basic understanding of CRCs.
        // Code generated by http://ghsi.de/CRC/index.php
        // ==========================================================================
        // CRC Generation Unit - Linear Feedback Shift Register implementation
        // (c) Kay Gorontzi, GHSi.de, distributed under the terms of LGPL
        // ==========================================================================
        verbprintf(4, "FMS CRC BIT: %1x\n", ((message >> (16+i)) & 1));

        doinvert = ((message >> (16+i)) & 1) ^ crc[6];         // XOR required?

        crc[6] = crc[5] ^ doinvert;
        crc[5] = crc[4];
        crc[4] = crc[3];
        crc[3] = crc[2];
        crc[2] = crc[1] ^ doinvert;
        crc[1] = crc[0];
        crc[0] = doinvert;

        //fms_print_crc(crc);
    }

    for (i=0; i<7; ++i)
    {
        if (crc[i])
        {
            fms_print_crc(crc);
            return 0;
        }
    }
    return 1;
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
    // uint8_t model;       // Baustufenkennung
    uint8_t direction;   // Richtungskennung
    uint8_t short_info;  // taktische Kurzinformation
    uint8_t crc;         // Redundanz

    fms_print_message_hex(message);

    verbprintf(0, "FMS: %08x%04x (", message >> 32, ((uint32_t)message >> 16));

    service_id = (message >> 16) & 0xF;
    fms_disp_service_id(service_id);

    state_id = (message >> 20) & 0xF;
    loc_id = (message >> 24) & 0xFF;
    fms_disp_state_id(state_id, loc_id);
    fms_disp_loc_id(loc_id);

    vehicle_id = (message >> 32) & 0xFFFF;
    fms_disp_vehicle_id(vehicle_id);

    state = (message >> 48) & 0xF;

    // model = (message >> 49) & 0x1;
    direction = (message >> 50) & 0x1;
    fms_disp_state(state, direction);

    fms_disp_direction(direction);

    short_info = (message >> 51) & 0x3;
    fms_disp_shortinfo(short_info);

    crc = (message >> 54) & 0x3F;

    verbprintf(0, ") ");

    if (fms_is_crc_correct(message))
    {
        verbprintf(0, "CRC correct");
        if (message & 1)
        {
            verbprintf(0, " AFTER SWAPPING ONE BIT");
        }
    }
    else
    {
        verbprintf(0, "CRC INCORRECT (%x)", crc);
    }
    verbprintf(0, "\n");
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
    uint64_t msg;
    int i;

    // General note on performance and logic: For the generation of the
    // variable that tracks if a SYNC-frame has been received, we use
    // a << since it is significantly faster (5s vs. 16s on a 20m test wave file)
    // For the message itself, we use a >> since that makes the message easier to decode

    // Append the bit to our "state machine tracker" so we can properly detect sync bits
    s->l2.fmsfsk.rxstate = ((s->l2.fmsfsk.rxstate << 1) & 0x000FFFFE) | bit;

    // Check if the sync pattern is in the buffer
    if ((s->l2.fmsfsk.rxstate & 0x0007FFFF) == 0x7FF1A)
    {
        verbprintf(1, "FMS ->SYNC<-\n");
        s->l2.fmsfsk.rxbitstream = 0; // reset RX buffer
        s->l2.fmsfsk.rxbitcount = 1;  // > 1 means we have a valid SYNC
    }

    // If we have a valid SYNC, record the message by appending it (from the left) to the RX buffer
    else if (s->l2.fmsfsk.rxbitcount >= 1) {
        verbprintf(4, "FMS BIT: %1x\n", bit);

        s->l2.fmsfsk.rxbitstream = (s->l2.fmsfsk.rxbitstream >> 1) | ((uint64_t) bit << 63);
        s->l2.fmsfsk.rxbitcount++;

        // Wait until message has been completely received. If so, decode and display it and reset
        if (s->l2.fmsfsk.rxbitcount == 49)
        {
            // If the CRC check fails, try to fix a one bit error
            if (!fms_is_crc_correct(s->l2.fmsfsk.rxbitstream))
            {
                i = 0;
                msg = s->l2.fmsfsk.rxbitstream;
                while (i <= 47)
                {
                    if (fms_is_crc_correct(msg ^ (1 << (i+16))))
                    {
                        verbprintf(2, "FMS was able to correct a one bit error by swapping bit %d Original packet:\n", i);
                        fms_disp_packet(s->l2.fmsfsk.rxbitstream);
                        s->l2.fmsfsk.rxbitstream = (msg ^ (1 << (i+16))) | 1; // lowest bit set means that the CRC has been corrected by us
                        break;
                    }
                    i++;
                }
                if (i == 48)
                {
                    verbprintf(2, "FMS: unable to correct CRC error\n");
                }
            }

            fms_disp_packet(s->l2.fmsfsk.rxbitstream);
            s->l2.fmsfsk.rxbitcount = 0; // Reset counter, meaning "no valid SYNC yet"
            s->l2.fmsfsk.rxstate = 0;    // Reset message input buffer
        }
    }
}

/* ---------------------------------------------------------------------- */
