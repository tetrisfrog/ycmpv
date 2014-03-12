/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This file contains functions interacting with the CoreAudio framework
 * that are not specific to the AUHAL. These are split in a separate file for
 * the sake of readability. In the future the could be used by other AOs based
 * on CoreAudio but not the AUHAL (such as using AudioQueue services).
 */

#include "audio/out/ao_coreaudio_utils.h"
#include "audio/out/ao_coreaudio_properties.h"
#include "osdep/timer.h"

char *fourcc_repr(void *talloc_ctx, uint32_t code)
{
    // Extract FourCC letters from the uint32_t and finde out if it's a valid
    // code that is made of letters.
    char fcc[4] = {
        (code >> 24) & 0xFF,
        (code >> 16) & 0xFF,
        (code >> 8)  & 0xFF,
        code         & 0xFF,
    };

    bool valid_fourcc = true;
    for (int i = 0; i < 4; i++)
        if (!isprint(fcc[i]))
            valid_fourcc = false;

    char *repr;
    if (valid_fourcc)
        repr = talloc_asprintf(talloc_ctx, "'%c%c%c%c'",
                               fcc[0], fcc[1], fcc[2], fcc[3]);
    else
        repr = talloc_asprintf(NULL, "%d", code);

    return repr;
}

bool check_ca_st(struct ao *ao, int level, OSStatus code, const char *message)
{
    if (code == noErr) return true;

    char *error_string = fourcc_repr(NULL, code);
    mp_msg(ao->log, level, "%s (%s)\n", message, error_string);
    talloc_free(error_string);

    return false;
}

static const int speaker_map[][2] = {
    { kAudioChannelLabel_Left,                 MP_SPEAKER_ID_FL   },
    { kAudioChannelLabel_Right,                MP_SPEAKER_ID_FR   },
    { kAudioChannelLabel_Center,               MP_SPEAKER_ID_FC   },
    { kAudioChannelLabel_LFEScreen,            MP_SPEAKER_ID_LFE  },
    { kAudioChannelLabel_LeftSurround,         MP_SPEAKER_ID_BL   },
    { kAudioChannelLabel_RightSurround,        MP_SPEAKER_ID_BR   },
    { kAudioChannelLabel_LeftCenter,           MP_SPEAKER_ID_FLC  },
    { kAudioChannelLabel_RightCenter,          MP_SPEAKER_ID_FRC  },
    { kAudioChannelLabel_CenterSurround,       MP_SPEAKER_ID_BC   },
    { kAudioChannelLabel_LeftSurroundDirect,   MP_SPEAKER_ID_SL   },
    { kAudioChannelLabel_RightSurroundDirect,  MP_SPEAKER_ID_SR   },
    { kAudioChannelLabel_TopCenterSurround,    MP_SPEAKER_ID_TC   },
    { kAudioChannelLabel_VerticalHeightLeft,   MP_SPEAKER_ID_TFL  },
    { kAudioChannelLabel_VerticalHeightCenter, MP_SPEAKER_ID_TFC  },
    { kAudioChannelLabel_VerticalHeightRight,  MP_SPEAKER_ID_TFR  },
    { kAudioChannelLabel_TopBackLeft,          MP_SPEAKER_ID_TBL  },
    { kAudioChannelLabel_TopBackCenter,        MP_SPEAKER_ID_TBC  },
    { kAudioChannelLabel_TopBackRight,         MP_SPEAKER_ID_TBR  },

    // unofficial extensions
    { kAudioChannelLabel_RearSurroundLeft,     MP_SPEAKER_ID_SDL  },
    { kAudioChannelLabel_RearSurroundRight,    MP_SPEAKER_ID_SDR  },
    { kAudioChannelLabel_LeftWide,             MP_SPEAKER_ID_WL   },
    { kAudioChannelLabel_RightWide,            MP_SPEAKER_ID_WR   },
    { kAudioChannelLabel_LFE2,                 MP_SPEAKER_ID_LFE2 },

    { kAudioChannelLabel_HeadphonesLeft,       MP_SPEAKER_ID_DL   },
    { kAudioChannelLabel_HeadphonesRight,      MP_SPEAKER_ID_DR   },

    { kAudioChannelLabel_Unknown,              -1 },
};

static int ca_label_to_mp_speaker_id(AudioChannelLabel label)
{
    for (int i = 0; speaker_map[i][0] != kAudioChannelLabel_Unknown; i++)
        if (speaker_map[i][0] == label)
            return speaker_map[i][1];
    return -1;
}

static bool ca_bitmap_from_ch_desc(struct ao *ao, AudioChannelLayout *layout,
                                   uint32_t *bitmap)
{
    // If the channel layout uses channel descriptions, from my
    // exepriments there are there three possibile cases:
    // * The description has a label kAudioChannelLabel_Unknown:
    //   Can't do anything about this (looks like non surround
    //   layouts are like this).
    // * The description uses positional information: this in
    //   theory could be used but one would have to map spatial
    //   positions to labels which is not really feasible.
    // * The description has a well known label which can be mapped
    //   to the waveextensible definition: this is the kind of
    //   descriptions we process here.
    size_t ch_num = layout->mNumberChannelDescriptions;
    bool all_channels_valid = true;

    for (int j=0; j < ch_num && all_channels_valid; j++) {
        AudioChannelLabel label = layout->mChannelDescriptions[j].mChannelLabel;
        const int mp_speaker_id = ca_label_to_mp_speaker_id(label);
        if (mp_speaker_id < 0) {
            MP_VERBOSE(ao, "channel label=%d unusable to build channel "
                           "bitmap, skipping layout\n", label);
            all_channels_valid = false;
        } else {
            *bitmap |= 1ULL << mp_speaker_id;
        }
    }

    return all_channels_valid;
}

static bool ca_bitmap_from_ch_tag(struct ao *ao, AudioChannelLayout *layout,
                                  uint32_t *bitmap)
{
    // This layout is defined exclusively by it's tag. Use the Audio
    // Format Services API to try and convert it to a bitmap that
    // mpv can use.
    uint32_t bitmap_size = sizeof(uint32_t);

    AudioChannelLayoutTag tag = layout->mChannelLayoutTag;
    OSStatus err = AudioFormatGetProperty(
        kAudioFormatProperty_BitmapForLayoutTag,
        sizeof(AudioChannelLayoutTag), &tag,
        &bitmap_size, bitmap);
    if (err != noErr) {
        MP_VERBOSE(ao, "channel layout tag=%d unusable to build channel "
                       "bitmap, skipping layout\n", tag);
        return false;
    } else {
        return true;
    }
}

void ca_bitmaps_from_layouts(struct ao *ao,
                             AudioChannelLayout *layouts, size_t n_layouts,
                             uint32_t **bitmaps, size_t *n_bitmaps)
{
    *n_bitmaps = 0;
    *bitmaps = talloc_array_size(NULL, sizeof(uint32_t), n_layouts);

    for (int i=0; i < n_layouts; i++) {
        uint32_t bitmap = 0;

        switch (layouts[i].mChannelLayoutTag) {
        case kAudioChannelLayoutTag_UseChannelBitmap:
            (*bitmaps)[(*n_bitmaps)++] = layouts[i].mChannelBitmap;
            break;

        case kAudioChannelLayoutTag_UseChannelDescriptions:
            if (ca_bitmap_from_ch_desc(ao, &layouts[i], &bitmap))
                (*bitmaps)[(*n_bitmaps)++] = bitmap;
            break;

        default:
            if (ca_bitmap_from_ch_tag(ao, &layouts[i], &bitmap))
                (*bitmaps)[(*n_bitmaps)++] = bitmap;
        }
    }
}
