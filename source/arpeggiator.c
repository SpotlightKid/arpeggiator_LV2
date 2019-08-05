#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/forge.h>
#include <lv2/lv2plug.in/ns/ext/log/logger.h>
#include <lv2/lv2plug.in/ns/ext/midi/midi.h>
#include "lv2/lv2plug.in/ns/ext/time/time.h"
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>

#ifndef DEBUG
#define DEBUG 0
#endif
#define debug_print(...) \
    ((void)((DEBUG) ? fprintf(stderr, __VA_ARGS__) : 0))

#define NUM_VOICES 16
#define PLUGIN_URI "http://bramgiesen.com/arpeggiator"


// Struct for a 3 byte MIDI event
typedef struct {
    LV2_Atom_Event event;
    uint8_t        msg[3];
} LV2_Atom_MIDI;


typedef enum {
    MIDI_IN = 0,
    MIDI_OUT,
    BPM_PORT,
    ARP_MODE,
    LATCH_MODE,
    DIVISIONS_PORT,
    SYNC_PORT,
    CONTROL_PORT,   
    NOTELENGTH,
    OCTAVESPREAD,
    OCTAVEMODE,
    VELOCITYPATTERNLENGTH,
    PATTERNVEL1,
    PATTERNVEL2,
    PATTERNVEL3,
    PATTERNVEL4,
    PATTERNVEL5,
    PATTERNVEL6,
    PATTERNVEL7,
    PATTERNVEL8
} PortIndex;


typedef struct {
    LV2_URID atom_Blank;
    LV2_URID atom_Float;
    LV2_URID atom_Object;
    LV2_URID atom_Path;
    LV2_URID atom_Resource;
    LV2_URID atom_Sequence;
    LV2_URID time_Position;
    LV2_URID time_barBeat;
    LV2_URID time_beatsPerMinute;
    LV2_URID time_speed;
} ClockURIs;

typedef struct {

    LV2_URID_Map*          map; // URID map feature
    LV2_Log_Log* 	       log;
    LV2_Log_Logger      logger; // Logger API
    ClockURIs             uris; // Cache of mapped URIDs

    // URIDs
    LV2_URID urid_midiEvent;

    const LV2_Atom_Sequence* MIDI_in;
    LV2_Atom_Sequence*       MIDI_out;

    float     divisions;
    double    samplerate;
    int       prevSync;
    LV2_Atom_Sequence* control;
    // Variables to keep track of the tempo information sent by the host
    float     bpm; // Beats per minute (tempo)
    uint32_t  pos;
    uint32_t  period;
    uint32_t  h_wavelength;
    uint8_t   midi_notes[NUM_VOICES];
    uint32_t  noteoff_buffer[NUM_VOICES][2];
    size_t    midi_index;
    size_t    active_notes_index;
    int       note_played;
    size_t    active_notes;
    size_t    pattern_index;
    int       octave_index;
    bool      triggered;
    bool      octave_up;
    bool      arp_up;
    bool      latch_playing;
    float     speed; // Transport speed (usually 0=stop, 1=play)
    float     prevSpeed;
    float     beatInMeasure;
    float     previous_latch;
    float   **velocity_pattern[8];

    float 	  elapsed_len; // Frames since the start of the last click
    uint32_t  wave_offset; // Current play offset in the wave
    int       previousOctaveMode;
    
    // Envelope parameters
    uint32_t  attack_len;
    uint32_t  decay_len;

    float*    changeBpm;
    float*    arp_mode;
    float*    latch_mode;
    float*    changedDiv;
    int*   	  sync;
    float*    note_length;
    float*    octaveSpreadParam;
    float*    octaveModeParam;
    float*    velocityPatternLengthParam;
    float*    patternVel1Param;
    float*    patternVel2Param;
    float*    patternVel3Param;
    float*    patternVel4Param;
    float*    patternVel5Param;
    float*    patternVel6Param;
    float*    patternVel7Param;
    float*    patternVel8Param;
} Arpeggiator;



static uint8_t 
octaveHandler(Arpeggiator* self)
{
    uint8_t octave = 0; 

    int octaveMode = *self->octaveModeParam;

    if (octaveMode != self->previousOctaveMode) {
        switch (octaveMode) 
        {
            case 0:
                self->octave_index = self->note_played % (int)*self->octaveSpreadParam;
                break;
            case 1:
                self->octave_index = self->note_played % (int)*self->octaveSpreadParam;
                self->octave_index = (int)*self->octaveSpreadParam;
                break;
            case 2:
                self->octave_index = self->note_played % (int)(*self->octaveSpreadParam * 2);
                if (self->octave_index > (int)*self->octaveSpreadParam) {
                    self->octave_index = abs((int)*self->octaveSpreadParam - (self->octave_index - (int)*self->octaveSpreadParam)) % (int)*self->octaveSpreadParam; 
                }
                self->octave_up = !self->octave_up;
                break;
            case 3:
                self->octave_index = (int)*self->octaveSpreadParam;
                self->octave_up = !self->octave_up;
                break;
        }
        self->previousOctaveMode = octaveMode;
    }

    if (*self->octaveSpreadParam > 1) {
        switch (octaveMode)
        {
            case 0:
                octave = 12 * self->octave_index; 
                self->octave_index = (self->octave_index + 1) % (int)*self->octaveSpreadParam;
                break;
            case 1:
                octave = 12 * self->octave_index; 
                self->octave_index--;
                self->octave_index = (self->octave_index < 0) ? (int)*self->octaveSpreadParam - 1 : self->octave_index;
                debug_print("octave index = %i\n", self->octave_index);
                break;
            case 2:
                octave = 12 * self->octave_index; 

                if (self->octave_up) {
                    debug_print("OCTAVE UP\n");
                    self->octave_index++;
                    self->octave_up = (self->octave_index >= (int)*self->octaveSpreadParam - 1) ? false : true;
                } else {
                    debug_print("OCTAVE DOWN\n");
                    self->octave_index--;
                    self->octave_up = (self->octave_index <= 0) ? true : false;
                }
                debug_print("octave index = %i\n", self->octave_index);
                break;
            case 3:
                octave = 12 * self->octave_index; 
                if (!self->octave_up) {
                    self->octave_index--;
                    self->octave_up = (self->octave_index <= 0) ? true : false;
                } else {
                    self->octave_index = (self->octave_index + 1) % (int)*self->octaveSpreadParam;
                    self->octave_up = (self->octave_index >= (int)*self->octaveSpreadParam - 1) ? false : true;
                }
                break;
        }
    } else {
        self->octave_index = 0;
    }

    return octave;
}



static LV2_Atom_MIDI
createMidiEvent(Arpeggiator* self, uint8_t status, uint8_t note, uint8_t velocity)
{
    LV2_Atom_MIDI msg;
    memset(&msg, 0, sizeof(LV2_Atom_MIDI));

    msg.event.body.size = 3;
    msg.event.body.type = self->urid_midiEvent;

    msg.msg[0] = status;
    msg.msg[1] = note;
    msg.msg[2] = velocity;

    return msg;
}



static void
handleNoteOn(Arpeggiator* self, const uint32_t outCapacity)
{
    size_t searched_voices = 0;
    bool   note_found = false;
    static int last_note_played = 0;
    while (!note_found && searched_voices < NUM_VOICES)
    {
        if (self->midi_notes[self->note_played] > 0
                && self->midi_notes[self->note_played] < 128)
        {
            uint8_t octave = octaveHandler(self);
            uint8_t velocity = (uint8_t)**self->velocity_pattern[self->pattern_index];
            self->pattern_index = (self->pattern_index + 1) % (int)*self->velocityPatternLengthParam; 

            //create MIDI note on message
            uint8_t midi_note = self->midi_notes[self->note_played] + octave;

            LV2_Atom_MIDI onMsg = createMidiEvent(self, 144, midi_note, velocity);
            lv2_atom_sequence_append_event(self->MIDI_out, outCapacity, (LV2_Atom_Event*)&onMsg);
            self->noteoff_buffer[self->active_notes_index][0] = (uint32_t)midi_note;
            self->active_notes_index = (self->active_notes_index + 1) % NUM_VOICES;
            last_note_played = self->note_played;
            //self->note_played = (self->note_played + 1) % NUM_VOICES;
            note_found = true;
        }
        if (*self->arp_mode == 0) {
        self->note_played = (self->note_played + 1) % NUM_VOICES;
        } else{
            if (self->arp_up) {
                self->note_played++;
                if (self->note_played >= NUM_VOICES - 1) {
                   self->arp_up = false; 
                   self->note_played = (self->active_notes > 1) ? last_note_played - 1 : last_note_played;
                }
            } else {
                self->note_played--;
                self->arp_up = (self->note_played <= 0) ? true : false;
            }
        } 
        searched_voices++;
    }
}



static void
handleNoteOff(Arpeggiator* self, const uint32_t outCapacity)
{
    for (size_t i = 0; i < NUM_VOICES; i++) {
        if (self->noteoff_buffer[i][0] > 0) {
            self->noteoff_buffer[i][1] += 1;
            if (self->noteoff_buffer[i][1] > (uint32_t)(self->period * *self->note_length)) {
                LV2_Atom_MIDI offMsg = createMidiEvent(self, 128, (uint8_t)self->noteoff_buffer[i][0], 0);
                lv2_atom_sequence_append_event(self->MIDI_out, outCapacity, (LV2_Atom_Event*)&offMsg);
                self->noteoff_buffer[i][0] = 0;
                self->noteoff_buffer[i][1] = 0;
            }
        }
    }
}


static void
connect_port(LV2_Handle instance,
        uint32_t   port,
        void*      data)
{
    Arpeggiator* self = (Arpeggiator*)instance;

    switch ((PortIndex)port) {
        case MIDI_IN:
            self->MIDI_in    = (const LV2_Atom_Sequence*)data;
            break;
        case MIDI_OUT:
            self->MIDI_out   = (LV2_Atom_Sequence*)data;
            break;
        case BPM_PORT:
            self->changeBpm = (float*)data;
            break;
        case ARP_MODE:
            self->arp_mode = (float*)data;
            break;
        case LATCH_MODE:
            self->latch_mode = (float*)data;
            break;
        case DIVISIONS_PORT:
            self->changedDiv = (float*)data;
            break;
        case SYNC_PORT:
            self->sync = (int*)data;
            break;
        case CONTROL_PORT:
            self->control = (LV2_Atom_Sequence*)data;
            break;
        case NOTELENGTH:
            self->note_length = (float*)data;
            break;
        case OCTAVESPREAD:
            self->octaveSpreadParam = (float*)data;
            break;
        case OCTAVEMODE:
            self->octaveModeParam  = (float*)data;
            break;
        case VELOCITYPATTERNLENGTH:
            self->velocityPatternLengthParam = (float*)data;
            break;
        case PATTERNVEL1:
            self->patternVel1Param = (float*)data;
            break;
        case PATTERNVEL2:
            self->patternVel2Param = (float*)data;
            break;
        case PATTERNVEL3:
            self->patternVel3Param = (float*)data;
            break;
        case PATTERNVEL4:
            self->patternVel4Param = (float*)data;
            break;
        case PATTERNVEL5:
            self->patternVel5Param = (float*)data;
            break;
        case PATTERNVEL6:
            self->patternVel6Param = (float*)data;
            break;
        case PATTERNVEL7:
            self->patternVel7Param = (float*)data;
            break;
        case PATTERNVEL8:
            self->patternVel8Param = (float*)data;
            break;
    }
}



static void
activate(LV2_Handle instance)
{
    Arpeggiator* self = (Arpeggiator*)instance;

    self->bpm = *self->changeBpm;
    self->divisions =*self->changedDiv;
    self->pos = 0;
}



static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
        double                    rate,
        const char*               bundle_path,
        const LV2_Feature* const* features)
{
    Arpeggiator* self = (Arpeggiator*)calloc(1, sizeof(Arpeggiator));
    if (!self)
    {
        return NULL;
    }

    for (uint32_t i=0; features[i]; ++i)
    {
        if (!strcmp (features[i]->URI, LV2_URID__map))
        {
            self->map = (LV2_URID_Map*)features[i]->data;
        }
        else if (!strcmp (features[i]->URI, LV2_LOG__log))
        {
            self->log = (LV2_Log_Log*)features[i]->data;
        }
    }

    lv2_log_logger_init (&self->logger, self->map, self->log);

    if (!self->map) {
        lv2_log_error (&self->logger, "StepSeq.lv2 error: Host does not support urid:map\n");
        free (self);
        return NULL;
    }


    // Map URIS
    ClockURIs* const    uris  = &self->uris;
    LV2_URID_Map* const map   = self->map;
    self->urid_midiEvent      = map->map(map->handle, LV2_MIDI__MidiEvent);
    uris->atom_Blank          = map->map(map->handle, LV2_ATOM__Blank);
    uris->atom_Float          = map->map(map->handle, LV2_ATOM__Float);
    uris->atom_Object         = map->map(map->handle, LV2_ATOM__Object);
    uris->atom_Path           = map->map(map->handle, LV2_ATOM__Path);
    uris->atom_Resource       = map->map(map->handle, LV2_ATOM__Resource);
    uris->atom_Sequence       = map->map(map->handle, LV2_ATOM__Sequence);
    uris->time_Position       = map->map(map->handle, LV2_TIME__Position);
    uris->time_barBeat        = map->map(map->handle, LV2_TIME__barBeat);
    uris->time_beatsPerMinute = map->map(map->handle, LV2_TIME__beatsPerMinute);
    uris->time_speed          = map->map(map->handle, LV2_TIME__speed);

    debug_print("DEBUGING");
    self->samplerate = rate;
    self->prevSync   = 0; 
    self->beatInMeasure = 0;
    self->prevSpeed = 0;
    self->midi_index = 0;
    self->triggered = false;
    self->octave_up = false;
    self->arp_up    = true;
    self->active_notes_index = 0;
    self->note_played = 0;
    self->active_notes = 0;
    self->pattern_index = 0;
    self->previousOctaveMode = 0;
    self->octave_index = 0;
    self->previous_latch = 0;
    self->latch_playing = false;

    for (unsigned i = 0; i < NUM_VOICES; i++) {
        self->midi_notes[i] = 0;
    }
    for (unsigned i = 0; i < NUM_VOICES; i++) {
        for (unsigned x = 0; x < 2; x++) {
            self->noteoff_buffer[i][x] = 0;
        }
    }
    
    self->velocity_pattern[0]  = &self->patternVel1Param;
    self->velocity_pattern[1]  = &self->patternVel2Param;
    self->velocity_pattern[2]  = &self->patternVel3Param;
    self->velocity_pattern[3]  = &self->patternVel4Param;
    self->velocity_pattern[4]  = &self->patternVel5Param;
    self->velocity_pattern[5]  = &self->patternVel6Param;
    self->velocity_pattern[6]  = &self->patternVel7Param;
    self->velocity_pattern[7]  = &self->patternVel8Param;

    return (LV2_Handle)self;
}



// Update the current position based on a host message.  This is called by
// run() when a time:Position is received.

static void
update_position(Arpeggiator* self, const LV2_Atom_Object* obj)
{
    const ClockURIs* uris = &self->uris;

    // Received new transport position/speed
    LV2_Atom *beat = NULL, *bpm = NULL, *speed = NULL;
    lv2_atom_object_get(obj,
            uris->time_barBeat, &beat,
            uris->time_beatsPerMinute, &bpm,
            uris->time_speed, &speed,
            NULL);
    if (bpm && bpm->type == uris->atom_Float)
    {
        // Tempo changed, update BPM
        self->bpm = ((LV2_Atom_Float*)bpm)->body;
    }
    if (speed && speed->type == uris->atom_Float)
    {
        // Speed changed, e.g. 0 (stop) to 1 (play)
        self->speed = ((LV2_Atom_Float*)speed)->body;
    }
    if (beat && beat->type == uris->atom_Float)
    {
        // Received a beat position, synchronise
        // This hard sync may cause clicks, a real plugin would be more graceful
        const float frames_per_beat = (self->samplerate * (60.0f / (self->bpm * self->divisions)));
        const float bar_beats       = (((LV2_Atom_Float*)beat)->body * self->divisions);
        const float beat_beats      = bar_beats - floorf(bar_beats);
        self->beatInMeasure         = ((LV2_Atom_Float*)beat)->body; 
        self->elapsed_len           = beat_beats * frames_per_beat;
    }
}



static uint32_t 
resetPhase(Arpeggiator* self)
{
    uint32_t pos = (uint32_t)fmod(self->samplerate * (60.0f / self->bpm) * self->beatInMeasure, (self->samplerate * (60.0f / (self->bpm * (self->divisions / 2.0f)))));

    return pos;
}


static void
run(LV2_Handle instance, uint32_t n_samples)
{
    Arpeggiator* self = (Arpeggiator*)instance;

    const ClockURIs* uris = &self->uris;
    const LV2_Atom_Sequence* in = self->control;

    for (const LV2_Atom_Event* ev = lv2_atom_sequence_begin(&in->body);
            !lv2_atom_sequence_is_end(&in->body, in->atom.size, ev);
            ev = lv2_atom_sequence_next(ev)) {

        if (ev->body.type == uris->atom_Object ||
                ev->body.type == uris->atom_Blank) {
            const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
            if (obj->body.otype == uris->time_Position) {
                update_position(self, obj);
            }
        }
    }

    self->MIDI_out->atom.type = self->MIDI_in->atom.type;

    const uint32_t out_capacity = self->MIDI_out->atom.size;

    // Write an empty Sequence header to the output
    lv2_atom_sequence_clear(self->MIDI_out);

    // Read incoming events
    LV2_ATOM_SEQUENCE_FOREACH(self->MIDI_in, ev)
    {
        size_t search_note;
        if (ev->body.type == self->urid_midiEvent)
        {
            const uint8_t* const msg = (const uint8_t*)(ev + 1);

            const uint8_t channel = msg[0] & 0x0F;
            const uint8_t status  = msg[0] & 0xF0;

            uint8_t midi_note = msg[1];
            uint8_t note_to_find;
            size_t find_free_voice;
            bool voice_found;

            switch (status)
            {
            case LV2_MIDI_MSG_NOTE_ON:
                if (self->active_notes == 0 && *self->sync == 0 && !self->latch_playing) {
                    self->pos = 0;
                    self->octave_index = 0;
                    self->pattern_index = 0;
                    self->note_played = 0;
                    self->triggered = false;
                }
                if (self->active_notes == 0 && *self->latch_mode == 1) {
                    self->latch_playing = true;
                    for (unsigned i = 0; i < NUM_VOICES; i++) {
                        self->midi_notes[i] = 0;
                    }
                }
                self->active_notes++;
                find_free_voice = 0;
                voice_found = false;
                while (find_free_voice < NUM_VOICES && !voice_found)
                {
                    if (self->midi_notes[find_free_voice] == 0) {  
                        self->midi_notes[find_free_voice] = midi_note;
                        voice_found = true;
                    }
                    find_free_voice++;
                }
                break;
            case LV2_MIDI_MSG_NOTE_OFF:
                self->active_notes--;
                note_to_find = midi_note;
                search_note = 0;
                if (*self->latch_mode == 0) {
                    self->latch_playing = false;
                    while (search_note < NUM_VOICES) 
                    {
                        if (self->midi_notes[search_note] == note_to_find) 
                        {
                            self->midi_notes[search_note] = 0;
                            search_note = NUM_VOICES;
                        }
                        search_note++;
                    }
                }
                //remove notes from list
                break;
            default:
                break;
            }
        }
        //lv2_atom_sequence_append_event(self->MIDI_out,
        //        out_capacity, ev);
    }

    if (*self->latch_mode != self->previous_latch) {
        for (unsigned i = 0; i < NUM_VOICES; i++) {
            self->midi_notes[i] = 0;
        }
        self->previous_latch = *self->latch_mode;
    }

    for(uint32_t i = 0; i < n_samples; i ++) {
        //map bpm to host or to bpm parameter
        if (!*self->sync) {
            self->bpm = *self->changeBpm;
        } else {
            self->bpm = self->bpm;
        }
        //reset phase when playing starts or stops
        if (self->speed != self->prevSpeed) {
            self->pos = resetPhase(self);
            self->prevSpeed = self->speed;
        }
        //reset phase when sync is turned on
        if (*self->sync != self->prevSync) {
            self->pos = resetPhase(self);
            self->prevSync = *self->sync;
        }
        //reset phase when there is a new division
        if (self->divisions != *self->changedDiv) {
            self->divisions = *self->changedDiv;
            self->pos = resetPhase(self);
        }

        self->period = (uint32_t)(self->samplerate * (60.0f / (self->bpm * (self->divisions / 2.0f))));
        self->h_wavelength = (self->period/2.0f);

        if(self->pos >= self->period && i < n_samples) {
            self->pos = 0;
        } else {
            if(self->pos < self->h_wavelength && !self->triggered) {
                //trigger MIDI message
                handleNoteOn(self, out_capacity);
                self->triggered = true;
            } else if (self->pos > self->h_wavelength) {
                //set gate
                self->triggered = false;
            }
        }
        handleNoteOff(self, out_capacity);
        self->pos += 1;
    }
}



static void
deactivate(LV2_Handle instance)
{
}

static void
cleanup(LV2_Handle instance)
{
    free(instance);
}

static const void*
extension_data(const char* uri)
{
    return NULL;
}

static const LV2_Descriptor descriptor = {
    PLUGIN_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    extension_data
};

LV2_SYMBOL_EXPORT
    const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
    switch (index) {
        case 0:  return &descriptor;
        default: return NULL;
    }
}

