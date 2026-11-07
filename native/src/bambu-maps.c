#include "bambu-maps.h"

#include "printer-snapshot.h"

#include <stdio.h>
#include <string.h>

/* Stage and filament identifiers are factual tables derived from pybambu 1.0.1
 * (Apache-2.0). See THIRD_PARTY_NOTICES.md. */

struct stage_entry {
    int id;
    const char *name;
};

static const struct stage_entry stages[] = {
    {0, "Printing"},
    {1, "Auto bed leveling"},
    {2, "Heatbed preheating"},
    {3, "Sweeping XY mech mode"},
    {4, "Changing filament"},
    {5, "Paused (M400)"},
    {6, "Paused filament runout"},
    {7, "Heating hotend"},
    {8, "Calibrating extrusion"},
    {9, "Scanning bed surface"},
    {10, "Inspecting first layer"},
    {11, "Identifying build plate type"},
    {12, "Calibrating micro lidar"},
    {13, "Homing toolhead"},
    {14, "Cleaning nozzle tip"},
    {15, "Checking extruder temperature"},
    {16, "Paused user"},
    {17, "Paused front cover falling"},
    {18, "Calibrating micro lidar"},
    {19, "Calibrating extrusion flow"},
    {20, "Paused nozzle temperature malfunction"},
    {21, "Paused heat bed temperature malfunction"},
    {22, "Filament unloading"},
    {23, "Paused skipped step"},
    {24, "Filament loading"},
    {25, "Calibrating motor noise"},
    {26, "Paused AMS lost"},
    {27, "Paused low fan speed heat break"},
    {28, "Paused chamber temperature control error"},
    {29, "Cooling chamber"},
    {30, "Paused user G-code"},
    {31, "Motor noise showoff"},
    {32, "Paused nozzle filament covered detected"},
    {33, "Paused cutter error"},
    {34, "Paused first layer error"},
    {35, "Paused nozzle clog"},
    {-1, "Idle"},
    {255, "Idle"},
};

struct filament_entry {
    const char *id;
    const char *name;
};

static const struct filament_entry filaments[] = {
    {"GFB00", "Bambu ABS"},
    {"GFB01", "Bambu ASA"},
    {"GFN03", "Bambu PA-CF"},
    {"GFN05", "Bambu PA6-CF"},
    {"GFN04", "Bambu PAHT-CF"},
    {"GFC00", "Bambu PC"},
    {"GFT01", "Bambu PET-CF"},
    {"GFG00", "Bambu PETG Basic"},
    {"GFG50", "Bambu PETG-CF"},
    {"GFA11", "Bambu PLA Aero"},
    {"GFA00", "Bambu PLA Basic"},
    {"GFA03", "Bambu PLA Impact"},
    {"GFA07", "Bambu PLA Marble"},
    {"GFA01", "Bambu PLA Matte"},
    {"GFA02", "Bambu PLA Metal"},
    {"GFA05", "Bambu PLA Silk"},
    {"GFA08", "Bambu PLA Sparkle"},
    {"GFA09", "Bambu PLA Tough"},
    {"GFA50", "Bambu PLA-CF"},
    {"GFS03", "Bambu Support For PA/PET"},
    {"GFS02", "Bambu Support For PLA"},
    {"GFS01", "Bambu Support G"},
    {"GFS00", "Bambu Support W"},
    {"GFU01", "Bambu TPU 95A"},
    {"GFB99", "Generic ABS"},
    {"GFB98", "Generic ASA"},
    {"GFS98", "Generic HIPS"},
    {"GFN98", "Generic PA-CF"},
    {"GFN99", "Generic PA"},
    {"GFC99", "Generic PC"},
    {"GFG99", "Generic PETG"},
    {"GFG98", "Generic PETG-CF"},
    {"GFL99", "Generic PLA"},
    {"GFL95", "Generic PLA-High Speed"},
    {"GFL96", "Generic PLA Silk"},
    {"GFL98", "Generic PLA-CF"},
    {"GFS99", "Generic PVA"},
    {"GFU99", "Generic TPU"},
    {"GFL05", "Overture Matte PLA"},
    {"GFL04", "Overture PLA"},
    {"GFB60", "PolyLite ABS"},
    {"GFB61", "PolyLite ASA"},
    {"GFG60", "PolyLite PETG"},
    {"GFL00", "PolyLite PLA"},
    {"GFL01", "PolyTerra PLA"},
    {"GFL03", "eSUN PLA+"},
    {"GFSL99_01", "Generic PLA Silk"},
    {"GFSL99_12", "Generic PLA Silk"},
    {"GFA12", "Bambu PLA Glow"},
    {"GFT97", "Generic PPS"},
    {"GFT98", "Generic PPS-CF"},
    {"GFU00", "Bambu TPU 95A HF"},
};

void bambu_stage_name(int stage_id, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0)
        return;
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        if (stages[i].id == stage_id) {
            snprintf(out, capacity, "%s", stages[i].name);
            return;
        }
    }
    snprintf(out, capacity, "Stage %d", stage_id);
}

bool bambu_speed_name(int speed_level, char *out, size_t capacity)
{
    static const char *const names[] = {"Silent", "Standard", "Sport", "Ludicrous"};

    if (out == NULL || capacity == 0)
        return false;
    if (speed_level >= 1 && speed_level <= 4) {
        snprintf(out, capacity, "%s", names[speed_level - 1]);
        return true;
    }
    snprintf(out, capacity, "Speed %d", speed_level);
    return false;
}

void bambu_filament_name(const char *filament_id, const char *tray_type, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0)
        return;
    out[0] = '\0';
    if (filament_id != NULL && filament_id[0] != '\0') {
        for (size_t i = 0; i < sizeof(filaments) / sizeof(filaments[0]); i++) {
            if (strcmp(filaments[i].id, filament_id) == 0) {
                bambu_copy_text(out, capacity, filaments[i].name);
                return;
            }
        }
    }
    if (tray_type != NULL && tray_type[0] != '\0')
        bambu_copy_text(out, capacity, tray_type);
    else if (filament_id != NULL && filament_id[0] != '\0')
        bambu_copy_text(out, capacity, filament_id);
    else
        bambu_copy_text(out, capacity, "Unknown");
}
