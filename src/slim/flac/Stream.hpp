/*
 * Copyright 2017, Andrej Kislovskij
 *
 * This is PUBLIC DOMAIN software so use at your own risk as it comes
 * with no warranties. This code is yours to share, use and modify without
 * any restrictions or obligations.
 *
 * For more information see conwrap/LICENSE or refer refer to http://unlicense.org
 *
 * Author: gimesketvirtadieni at gmail dot com (Andrej Kislovskij)
 */

#pragma once

#include <cstddef>   // std::size_t
#include <FLAC++/encoder.h>
#include <optional>

#include "slim/log/log.hpp"
#include "slim/StreamWriter.hpp"
#include "slim/util/ExpandableBuffer.hpp"


namespace slim
{
	namespace flac
	{
		class Stream : public StreamWriter, public FLAC::Encoder::Stream
		{
			public:
				explicit Stream(StreamWriter* w, unsigned int c, unsigned int s, unsigned int b)
				: StreamWriter
				{
					std::move([&](auto* data, auto s) mutable
					{
						auto size{s};

						// do not feed encoder with more data if there is no room in transfer buffer
						if (getFreeBufferIndex().has_value())
						{
							size = encode(data, size);
						}
						else
						{
							LOG(WARNING) << LABELS{"flac"} << "Transfer buffer is full - skipping PCM chunk";
						}

						return size;
					})
				}
				, writerPtr{w}
				, channels{c}
				, sampleRate{s}
				, bitsPerSample{b}
				, bytesPerFrame{channels * (bitsPerSample >> 3)}
				, byteRate{sampleRate * bytesPerFrame}
				{
					auto ok{true};

					// do not validate whether the stream is bit-perfect to the original PCM data
					ok &= set_verify(false);
					ok &= set_compression_level(8);
					ok &= set_channels(channels);
					ok &= set_sample_rate(sampleRate);
					// TODO: FLAC does not support 32 bits per sample
					ok &= set_bits_per_sample(24);

					// choosing big enough number of expected samples for streaming purpose
					ok &= set_total_samples_estimate(0xFFFFFFFF);
/*
					// now add some metadata; we'll add some tags and a padding block
					if (ok)
					{
						if(
							(metadata[0] = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT)) == NULL ||
							(metadata[1] = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PADDING)) == NULL ||
							// there are many tag (vorbiscomment) functions but these are convenient for this particular use:
							!FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, "ARTIST", "Some Artist") ||
							!FLAC__metadata_object_vorbiscomment_append_comment(metadata[0], entry, false) || // copy=false: let metadata object take control of entry's allocated string
							!FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, "YEAR", "1984") ||
							!FLAC__metadata_object_vorbiscomment_append_comment(metadata[0], entry, false)
						) {
							fprintf(stderr, "ERROR: out of memory or tag error\n");
							ok = false;
						}

						metadata[1]->length = 1234; // set the padding length

						ok = encoder.set_metadata(metadata, 2);
					}
*/
					if (ok)
					{
						auto init_status{init()};
						if(init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
						{
							LOG(ERROR) << LABELS{"flac"} << FLAC__StreamEncoderInitStatusString[init_status];
							ok = false;
						}
					}

					if (!ok)
					{
						LOG(ERROR) << LABELS{"flac"} << "Initialization error";
					}
				}

				virtual ~Stream()
				{
					if (!finish())
					{
						LOG(ERROR) << LABELS{"flac"} << "Finish failed:" << get_state().as_cstring();
					}

					// now that encoding is finished, the metadata can be freed
					//FLAC__metadata_object_delete(metadata[0]);
					//FLAC__metadata_object_delete(metadata[1]);
				}

				Stream(const Stream&) = delete;             // non-copyable
				Stream& operator=(const Stream&) = delete;  // non-assignable
				Stream(Stream&&) = delete;                  // non-movable
				Stream& operator=(Stream&&) = delete;       // non-assign-movable

				virtual std::string getMIME()
				{
					return "audio/flac";
				}

				virtual void rewind(const std::streampos pos)
				{
					writerPtr->rewind(pos);
				}

				virtual void writeAsync(const void* data, const std::size_t size, WriteCallback callback) override
				{
					// TODO: temporary solution, implement async transfer
					write(data, size);
					callback(std::error_code(), size);
				}

				void writeHeader(std::uint32_t size = 0) {}

			protected:
				std::streamsize encode(const char* data, const std::streamsize s)
				{
					// TODO: avoid type conversion
					auto size{(std::size_t)s};
					auto samples{size >> 2};
					auto frames{samples >> 1};

					// checking if only 24 bits are used for PCM data
					auto scalingWarning{false};
					for (std::size_t i = 0; i < size; i += 4)
					{
						if (data[i])
						{
							// TODO: consider other place for this scaling to 24 bits
							const_cast<char*>(data)[i] = 0;
							if (!scalingWarning)
							{
								LOG(WARNING) << LABELS{"flac"} << "All 32-bits are used for PCM data, scaling to 24 bits as required for FLAC";
								scalingWarning = true;
							}
						}
					}

					// coverting data S32_LE to S24_LE by shifting data by 1 byte
					if (frames > 1 && !process_interleaved((const FLAC__int32*)(data + 1), frames - 1))
					{
						LOG(ERROR) << LABELS{"flac"} << get_state().as_cstring();
					}

					// handling the last frame separately; requred due to data shift
					if (frames > 0)
					{
						char lastFrame[8] =
						{
							data[samples * 4 - 7],
							data[samples * 4 - 6],
							data[samples * 4 - 5],
							0,
							data[samples * 4 - 3],
							data[samples * 4 - 2],
							data[samples * 4 - 1],
							0
						};

						if (!process_interleaved((const FLAC__int32*)lastFrame, 1))
						{
							LOG(ERROR) << LABELS{"flac"} << get_state().as_cstring();
						}
					}

					return size;
				}

				std::optional<std::size_t> getFreeBufferIndex()
				{
					std::optional<std::size_t> result{std::nullopt};
					auto                       size{buffers.size()};

					for (std::size_t i{0}; i < size; i++)
					{
						if (!buffers[i].size())
						{
							result = i;
							break;
						}
					}

					return result;
				}

				virtual ::FLAC__StreamEncoderWriteStatus write_callback(const FLAC__byte* data, std::size_t size, unsigned samples, unsigned current_frame) override
				{
					if (auto index{getFreeBufferIndex()}; index.has_value())
					{
						auto& buffer{buffers[index.value()]};

						// no need for capacity adjustment as it is done by .assign method
						buffer.assign(data, size);

						writerPtr->writeAsync(buffer.data(), buffer.size(), [&](const std::error_code& error, std::size_t transferred) mutable
						{
							buffer.size(0);

							// TODO: consider additional error processing
							if (error)
							{
								LOG(ERROR) << LABELS{"flac"} << "Error while transferring data (" << error.message() << ")";
							}
						});
					}
					else
					{
						LOG(WARNING) << LABELS{"flac"} << "Transfer buffer is full - skipping encoded chunk";
					}

					return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
				}

			private:
				StreamWriter* writerPtr;
				unsigned int  channels;
				unsigned int  sampleRate;
				unsigned int  bitsPerSample;
				unsigned int  bytesPerFrame;
				unsigned int  byteRate;

				// TODO: work in progress
				std::array<util::ExpandableBuffer, 10> buffers;
		};
	}
}
