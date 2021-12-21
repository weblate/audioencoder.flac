/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
#include <kodi/addon-instance/AudioEncoder.h>
#include <string.h>

static const size_t SAMPLES_BUF_SIZE = 1024 * 2;

class ATTR_DLL_LOCAL CEncoderFlac : public kodi::addon::CInstanceAudioEncoder
{
public:
  CEncoderFlac(const kodi::addon::IInstanceInfo& instance);
  ~CEncoderFlac() override;

  bool Start(const kodi::addon::AudioEncoderInfoTag& tag) override;
  ssize_t Encode(const uint8_t* stream, size_t numBytesRead) override;
  bool Finish() override;

private:
  static FLAC__StreamEncoderWriteStatus write_callback_flac(const FLAC__StreamEncoder* encoder,
                                                            const FLAC__byte buffer[],
                                                            size_t bytes,
                                                            unsigned samples,
                                                            unsigned current_frame,
                                                            void* client_data);
  static FLAC__StreamEncoderSeekStatus seek_callback_flac(const FLAC__StreamEncoder* encoder,
                                                          FLAC__uint64 absolute_byte_offset,
                                                          void* client_data);

  static FLAC__StreamEncoderTellStatus tell_callback_flac(const FLAC__StreamEncoder* encoder,
                                                          FLAC__uint64* absolute_byte_offset,
                                                          void* client_data);

  int64_t m_tellPos; ///< position for tell() callback
  FLAC__StreamEncoder* m_encoder;
  FLAC__StreamMetadata* m_metadata[2];
  FLAC__int32 m_samplesBuf[SAMPLES_BUF_SIZE];
};

CEncoderFlac::CEncoderFlac(const kodi::addon::IInstanceInfo& instance)
  : CInstanceAudioEncoder(instance), m_tellPos(0)
{
  m_metadata[0] = nullptr;
  m_metadata[1] = nullptr;

  m_encoder = FLAC__stream_encoder_new();
  if (m_encoder == nullptr)
    kodi::Log(ADDON_LOG_ERROR, "Failed to construct flac stream encoder");
}

CEncoderFlac::~CEncoderFlac()
{
  // free the metadata
  if (m_metadata[0])
    FLAC__metadata_object_delete(m_metadata[0]);
  if (m_metadata[1])
    FLAC__metadata_object_delete(m_metadata[1]);

  // free the encoder
  if (m_encoder)
    FLAC__stream_encoder_delete(m_encoder);
}

bool CEncoderFlac::Start(const kodi::addon::AudioEncoderInfoTag& tag)
{
  if (!m_encoder)
    return false;

  // we accept only 2 / 44100 / 16 atm
  if (tag.GetChannels() != 2 || tag.GetSamplerate() != 44100 || tag.GetBitsPerSample() != 16)
  {
    kodi::Log(ADDON_LOG_ERROR, "Invalid input format to encode");
    return false;
  }

  FLAC__bool ok = 1;

  ok &= FLAC__stream_encoder_set_verify(m_encoder, true);
  ok &= FLAC__stream_encoder_set_channels(m_encoder, tag.GetChannels());
  ok &= FLAC__stream_encoder_set_bits_per_sample(m_encoder, tag.GetBitsPerSample());
  ok &= FLAC__stream_encoder_set_sample_rate(m_encoder, tag.GetSamplerate());
  ok &= FLAC__stream_encoder_set_total_samples_estimate(m_encoder, tag.GetTrackLength() / 4);
  ok &= FLAC__stream_encoder_set_compression_level(m_encoder, kodi::addon::GetSettingInt("level"));

  // now add some metadata
  FLAC__StreamMetadata_VorbisComment_Entry entry;
  if (ok)
  {
    if ((m_metadata[0] = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT)) ==
            nullptr ||
        (m_metadata[1] = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PADDING)) == nullptr ||
        !FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, "ARTIST",
                                                                        tag.GetArtist().c_str()) ||
        !FLAC__metadata_object_vorbiscomment_append_comment(m_metadata[0], entry, false) ||
        !FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, "ALBUM",
                                                                        tag.GetAlbum().c_str()) ||
        !FLAC__metadata_object_vorbiscomment_append_comment(m_metadata[0], entry, false) ||
        !FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(
            &entry, "ALBUMARTIST", tag.GetAlbumArtist().c_str()) ||
        !FLAC__metadata_object_vorbiscomment_append_comment(m_metadata[0], entry, false) ||
        !FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, "TITLE",
                                                                        tag.GetTitle().c_str()) ||
        !FLAC__metadata_object_vorbiscomment_append_comment(m_metadata[0], entry, false) ||
        !FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, "GENRE",
                                                                        tag.GetGenre().c_str()) ||
        !FLAC__metadata_object_vorbiscomment_append_comment(m_metadata[0], entry, false) ||
        !FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(
            &entry, "TRACKNUMBER", std::to_string(tag.GetTrack()).c_str()) ||
        !FLAC__metadata_object_vorbiscomment_append_comment(m_metadata[0], entry, false) ||
        !FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(
            &entry, "DATE", tag.GetReleaseDate().c_str()) ||
        !FLAC__metadata_object_vorbiscomment_append_comment(m_metadata[0], entry, false) ||
        !FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, "COMMENT",
                                                                        tag.GetComment().c_str()) ||
        !FLAC__metadata_object_vorbiscomment_append_comment(m_metadata[0], entry, false))
    {
      ok = false;
    }
    else
    {
      m_metadata[1]->length = 4096;
      ok = FLAC__stream_encoder_set_metadata(m_encoder, m_metadata, 2);
    }
  }

  // initialize encoder in stream mode
  if (ok)
  {
    FLAC__StreamEncoderInitStatus init_status;
    init_status = FLAC__stream_encoder_init_stream(
        m_encoder, write_callback_flac, seek_callback_flac, tell_callback_flac, nullptr, this);
    if (init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
    {
      ok = false;
    }
  }

  if (!ok)
  {
    kodi::Log(ADDON_LOG_ERROR, "Failed to create flac stream encoder");
    return false;
  }

  return true;
}

ssize_t CEncoderFlac::Encode(const uint8_t* stream, size_t numBytesRead)
{
  if (!m_encoder)
    return 0;

  size_t nLeftSamples = numBytesRead / 2; // each sample takes 2 bytes (16 bits per sample)
  while (nLeftSamples > 0)
  {
    size_t nSamples = nLeftSamples > SAMPLES_BUF_SIZE ? SAMPLES_BUF_SIZE : nLeftSamples;

    // convert the packed little-endian 16-bit PCM samples into an interleaved FLAC__int32 buffer for libFLAC
    for (size_t i = 0; i < nSamples; i++)
    { // inefficient but simple and works on big- or little-endian machines.
      m_samplesBuf[i] = (FLAC__int32)(((FLAC__int16)(FLAC__int8)stream[2 * i + 1] << 8) |
                                      (FLAC__int16)stream[2 * i]);
    }

    // feed samples to encoder
    if (!FLAC__stream_encoder_process_interleaved(m_encoder, m_samplesBuf, nSamples / 2))
    {
      return 0;
    }

    nLeftSamples -= nSamples;
    stream += nSamples * 2; // skip processed samples
  }
  return numBytesRead; // consumed everything
}

bool CEncoderFlac::Finish()
{
  if (!m_encoder)
    return false;

  FLAC__stream_encoder_finish(m_encoder);
  return true;
}

FLAC__StreamEncoderWriteStatus CEncoderFlac::write_callback_flac(const FLAC__StreamEncoder* encoder,
                                                                 const FLAC__byte buffer[],
                                                                 size_t bytes,
                                                                 unsigned samples,
                                                                 unsigned current_frame,
                                                                 void* client_data)
{
  CEncoderFlac* context = static_cast<CEncoderFlac*>(client_data);
  if (context)
  {
    if (context->Write(static_cast<const uint8_t*>(buffer), bytes) == bytes)
    {
      context->m_tellPos += bytes;
      return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    }
  }
  return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
}

FLAC__StreamEncoderSeekStatus CEncoderFlac::seek_callback_flac(const FLAC__StreamEncoder* encoder,
                                                               FLAC__uint64 absolute_byte_offset,
                                                               void* client_data)
{
  CEncoderFlac* context = static_cast<CEncoderFlac*>(client_data);
  if (context)
  {
    if (context->Seek(static_cast<int64_t>(absolute_byte_offset), 0) == absolute_byte_offset)
    {
      context->m_tellPos = absolute_byte_offset;
      return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
    }
  }
  return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
}

FLAC__StreamEncoderTellStatus CEncoderFlac::tell_callback_flac(const FLAC__StreamEncoder* encoder,
                                                               FLAC__uint64* absolute_byte_offset,
                                                               void* client_data)
{
  // libFLAC will cope without a real tell callback
  CEncoderFlac* context = static_cast<CEncoderFlac*>(client_data);
  if (context)
  {
    *absolute_byte_offset = context->m_tellPos;
    return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
  }
  return FLAC__STREAM_ENCODER_TELL_STATUS_ERROR;
}

//------------------------------------------------------------------------------

class ATTR_DLL_LOCAL CMyAddon : public kodi::addon::CAddonBase
{
public:
  CMyAddon() = default;
  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance,
                              KODI_ADDON_INSTANCE_HDL& hdl) override;
};

ADDON_STATUS CMyAddon::CreateInstance(const kodi::addon::IInstanceInfo& instance,
                                      KODI_ADDON_INSTANCE_HDL& hdl)
{
  hdl = new CEncoderFlac(instance);
  return ADDON_STATUS_OK;
}

ADDONCREATOR(CMyAddon);
