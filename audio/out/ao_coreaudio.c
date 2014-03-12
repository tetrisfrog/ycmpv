/*
 * CoreAudio audio output driver for Mac OS X
 *
 * original copyright (C) Timothy J. Wood - Aug 2000
 * ported to MPlayer libao2 by Dan Christiansen
 *
 * Chris Roccati
 * Stefano Pigozzi
 *
 * The S/PDIF part of the code is based on the auhal audio output
 * module from VideoLAN:
 * Copyright (c) 2006 Derk-Jan Hartman <hartman at videolan dot org>
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * along with MPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * The MacOS X CoreAudio framework doesn't mesh as simply as some
 * simpler frameworks do.  This is due to the fact that CoreAudio pulls
 * audio samples rather than having them pushed at it (which is nice
 * when you are wanting to do good buffering of audio).
 */

#include "config.h"
#include "ao.h"
#include "internal.h"
#include "audio/format.h"
#include "osdep/timer.h"
#include "options/m_option.h"
#include "common/msg.h"
#include "audio/out/ao_coreaudio_properties.h"
#include "audio/out/ao_coreaudio_utils.h"

static void audio_pause(struct ao *ao);
static void audio_resume(struct ao *ao);

struct priv {
    AudioDeviceID device;   // selected device
    AudioUnit audio_unit;   // AudioUnit for lpcm output
    bool paused;

    // options
    int opt_device_id;
    int opt_list;
};

static OSStatus render_cb_lpcm(void *ctx, AudioUnitRenderActionFlags *aflags,
                              const AudioTimeStamp *ts, UInt32 bus,
                              UInt32 frames, AudioBufferList *buffer_list)
{
    struct ao *ao   = ctx;
    AudioBuffer buf = buffer_list->mBuffers[0];
    ao_read_data(ao, &buf.mData, frames, mp_time_us());
    return noErr;
}

static int control(struct ao *ao, enum aocontrol cmd, void *arg)
{
    struct priv *p = ao->priv;
    ao_control_vol_t *control_vol;
    OSStatus err;
    Float32 vol;
    switch (cmd) {
    case AOCONTROL_GET_VOLUME:
        control_vol = (ao_control_vol_t *)arg;
        err = AudioUnitGetParameter(p->audio_unit, kHALOutputParam_Volume,
                                    kAudioUnitScope_Global, 0, &vol);

        CHECK_CA_ERROR("could not get HAL output volume");
        control_vol->left = control_vol->right = vol * 100.0;
        return CONTROL_TRUE;

    case AOCONTROL_SET_VOLUME:
        control_vol = (ao_control_vol_t *)arg;
        vol = (control_vol->left + control_vol->right) / 200.0;
        err = AudioUnitSetParameter(p->audio_unit, kHALOutputParam_Volume,
                                    kAudioUnitScope_Global, 0, vol, 0);

        CHECK_CA_ERROR("could not set HAL output volume");
        return CONTROL_TRUE;

    } // end switch
    return CONTROL_UNKNOWN;

coreaudio_error:
    return CONTROL_ERROR;
}

static void print_list(struct ao *ao)
{
    char *help = talloc_strdup(NULL, "Available output devices:\n");

    AudioDeviceID *devs;
    size_t n_devs;

    OSStatus err =
        CA_GET_ARY(kAudioObjectSystemObject, kAudioHardwarePropertyDevices,
                   &devs, &n_devs);

    CHECK_CA_ERROR("Failed to get list of output devices.");

    for (int i = 0; i < n_devs; i++) {
        char *name;
        err = CA_GET_STR(devs[i], kAudioObjectPropertyName, &name);

        if (err == noErr)
            talloc_steal(devs, name);
        else
            name = "Unknown";

        help = talloc_asprintf_append(
                help, "  * %s (id: %" PRIu32 ")\n", name, devs[i]);
    }

    talloc_free(devs);

coreaudio_error:
    MP_INFO(ao, "%s", help);
    talloc_free(help);
}

static int init_audiounit(struct ao *ao, AudioStreamBasicDescription asbd);

static int init(struct ao *ao)
{
    OSStatus err;
    struct priv *p   = ao->priv;

    if (p->opt_list) print_list(ao);

    ao->per_application_mixer = true;
    ao->no_persistent_volume  = true;

    AudioDeviceID selected_device = 0;
    if (p->opt_device_id < 0) {
        // device not set by user, get the default one
        err = CA_GET(kAudioObjectSystemObject,
                     kAudioHardwarePropertyDefaultOutputDevice,
                     &selected_device);
        CHECK_CA_ERROR("could not get default audio device");
    } else {
        selected_device = p->opt_device_id;
    }

    if (mp_msg_test(ao->log, MSGL_V)) {
        char *name;
        err = CA_GET_STR(selected_device, kAudioObjectPropertyName, &name);
        CHECK_CA_ERROR("could not get selected audio device name");

        MP_VERBOSE(ao, "selected audio output device: %s (%" PRIu32 ")\n",
                       name, selected_device);

        talloc_free(name);
    }

    // Save selected device id
    p->device = selected_device;

    ao->format = af_fmt_from_planar(ao->format);

    AudioChannelLayout *layouts;
    size_t n_layouts;
    err = CA_GET_ARY_O(selected_device,
                       kAudioDevicePropertyPreferredChannelLayout,
                       &layouts, &n_layouts);
    CHECK_CA_ERROR("could not get audio device prefered layouts");

    uint32_t *bitmaps;
    size_t   n_bitmaps;

    ca_bitmaps_from_layouts(ao, layouts, n_layouts, &bitmaps, &n_bitmaps);
    talloc_free(layouts);

    struct mp_chmap_sel chmap_sel = {0};

    for (int i=0; i < n_bitmaps; i++) {
        struct mp_chmap chmap = {0};
        mp_chmap_from_lavc(&chmap, bitmaps[i]);
        mp_chmap_sel_add_map(&chmap_sel, &chmap);
    }

    talloc_free(bitmaps);

    if (ao->channels.num < 3 || n_bitmaps < 1)
        // If the input is not surround or we could not get any usable
        // bitmap from the hardware, default to waveext...
        mp_chmap_sel_add_waveext(&chmap_sel);

    if (!ao_chmap_sel_adjust(ao, &chmap_sel, &ao->channels))
        goto coreaudio_error;

    // Build ASBD for the input format
    AudioStreamBasicDescription asbd;
    asbd.mSampleRate       = ao->samplerate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mChannelsPerFrame = ao->channels.num;
    asbd.mBitsPerChannel   = af_fmt2bits(ao->format);
    asbd.mFormatFlags      = kAudioFormatFlagIsPacked;

    if ((ao->format & AF_FORMAT_POINT_MASK) == AF_FORMAT_F)
        asbd.mFormatFlags |= kAudioFormatFlagIsFloat;

    if ((ao->format & AF_FORMAT_SIGN_MASK) == AF_FORMAT_SI)
        asbd.mFormatFlags |= kAudioFormatFlagIsSignedInteger;

    if ((ao->format & AF_FORMAT_END_MASK) == AF_FORMAT_BE)
        asbd.mFormatFlags |= kAudioFormatFlagIsBigEndian;

    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerPacket = asbd.mBytesPerFrame =
        asbd.mFramesPerPacket * asbd.mChannelsPerFrame *
        (asbd.mBitsPerChannel / 8);

    return init_audiounit(ao, asbd);

coreaudio_error:
    return CONTROL_ERROR;
}

static int init_audiounit(struct ao *ao, AudioStreamBasicDescription asbd)
{
    OSStatus err;
    uint32_t size;
    struct priv *p = ao->priv;

    AudioComponentDescription desc = (AudioComponentDescription) {
        .componentType         = kAudioUnitType_Output,
        .componentSubType      = (p->opt_device_id < 0) ?
                                    kAudioUnitSubType_DefaultOutput :
                                    kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
        .componentFlags        = 0,
        .componentFlagsMask    = 0,
    };

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (comp == NULL) {
        MP_ERR(ao, "unable to find audio component\n");
        goto coreaudio_error;
    }

    err = AudioComponentInstanceNew(comp, &(p->audio_unit));
    CHECK_CA_ERROR("unable to open audio component");

    // Initialize AudioUnit
    err = AudioUnitInitialize(p->audio_unit);
    CHECK_CA_ERROR_L(coreaudio_error_component,
                     "unable to initialize audio unit");

    size = sizeof(AudioStreamBasicDescription);
    err = AudioUnitSetProperty(p->audio_unit,
                               kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 0, &asbd, size);

    CHECK_CA_ERROR_L(coreaudio_error_audiounit,
                     "unable to set the input format on the audio unit");

    //Set the Current Device to the Default Output Unit.
    err = AudioUnitSetProperty(p->audio_unit,
                               kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global, 0, &p->device,
                               sizeof(p->device));
    CHECK_CA_ERROR_L(coreaudio_error_audiounit,
                     "can't link audio unit to selected device");

    if (ao->channels.num > 2) {
        // No need to set a channel layout for mono and stereo inputs
        AudioChannelLayout acl = (AudioChannelLayout) {
            .mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap,
            .mChannelBitmap    = mp_chmap_to_waveext(&ao->channels)
        };

        err = AudioUnitSetProperty(p->audio_unit,
                                   kAudioUnitProperty_AudioChannelLayout,
                                   kAudioUnitScope_Input, 0, &acl,
                                   sizeof(AudioChannelLayout));
        CHECK_CA_ERROR_L(coreaudio_error_audiounit,
                         "can't set channel layout bitmap into audio unit");
    }

    AURenderCallbackStruct render_cb = (AURenderCallbackStruct) {
        .inputProc       = render_cb_lpcm,
        .inputProcRefCon = ao,
    };

    err = AudioUnitSetProperty(p->audio_unit,
                               kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &render_cb,
                               sizeof(AURenderCallbackStruct));

    CHECK_CA_ERROR_L(coreaudio_error_audiounit,
                     "unable to set render callback on audio unit");

    return CONTROL_OK;

coreaudio_error_audiounit:
    AudioUnitUninitialize(p->audio_unit);
coreaudio_error_component:
    AudioComponentInstanceDispose(p->audio_unit);
coreaudio_error:
    return CONTROL_ERROR;
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;
    AudioOutputUnitStop(p->audio_unit);
    AudioUnitUninitialize(p->audio_unit);
    AudioComponentInstanceDispose(p->audio_unit);
}

static void audio_pause(struct ao *ao)
{
    struct priv *p = ao->priv;
    OSErr err = AudioOutputUnitStop(p->audio_unit);
    CHECK_CA_WARN("can't stop audio unit");
}

static void audio_resume(struct ao *ao)
{
    struct priv *p = ao->priv;
    OSErr err = AudioOutputUnitStart(p->audio_unit);
    CHECK_CA_WARN("can't start audio unit");
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_coreaudio = {
    .description = "CoreAudio (OS X Audio Output)",
    .name      = "coreaudio",
    .uninit    = uninit,
    .init      = init,
    .control   = control,
    .pause     = audio_pause,
    .resume    = audio_resume,
    .priv_size = sizeof(struct priv),
    .options = (const struct m_option[]) {
        OPT_INT("device_id", opt_device_id, 0, OPTDEF_INT(-1)),
        OPT_FLAG("list", opt_list, 0),
        {0}
    },
};
