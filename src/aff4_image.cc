/*
Copyright 2014 Google Inc. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License"); you may not use
this file except in compliance with the License.  You may obtain a copy of the
License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied.  See the License for the
specific language governing permissions and limitations under the License.
*/

#include "lexicon.h"
#include "aff4_image.h"
#include <zlib.h>
#include <snappy.h>


AFF4ScopedPtr<AFF4Image> AFF4Image::NewAFF4Image(
    DataStore *resolver, const URN &image_urn, const URN &volume_urn) {
  AFF4ScopedPtr<AFF4Volume> volume = resolver->AFF4FactoryOpen<AFF4Volume>(
      volume_urn);

  if (!volume)
    return AFF4ScopedPtr<AFF4Image>();        /** Volume not known? */

  // Inform the volume that we have a new image stream contained within it.
  volume->children.insert(image_urn.SerializeToString());

  resolver->Set(image_urn, AFF4_TYPE, new URN(AFF4_IMAGE_TYPE));
  resolver->Set(image_urn, AFF4_STORED, new URN(volume_urn));

  return resolver->AFF4FactoryOpen<AFF4Image>(image_urn);
};


/**
 * Initializes this AFF4 object from the information stored in the resolver.
 *
 *
 * @return STATUS_OK if the object was successfully initialized.
 */
AFF4Status AFF4Image::LoadFromURN() {
  if (resolver->Get(urn, AFF4_STORED, volume_urn) != STATUS_OK) {
    return NOT_FOUND;
  };

  // Configure the stream parameters.
  XSDInteger value;

  if(resolver->Get(urn, AFF4_IMAGE_CHUNK_SIZE, value) == STATUS_OK) {
    chunk_size = value.value;
  };

  if(resolver->Get(urn, AFF4_IMAGE_CHUNKS_PER_SEGMENT, value) == STATUS_OK) {
    chunks_per_segment = value.value;
  };

  if(resolver->Get(urn, AFF4_STREAM_SIZE, value) == STATUS_OK) {
    size = value.value;
  };

  // Load the compression scheme. If it is not set we just default to ZLIB.
  URN compression_urn;
  if(STATUS_OK ==
     resolver->Get(urn, AFF4_IMAGE_COMPRESSION, compression_urn)) {
    compression = CompressionMethodFromURN(compression_urn);
    if (compression == AFF4_IMAGE_COMPRESSION_ENUM_UNKNOWN) {
      LOG(ERROR) << "Compression method " <<
          compression_urn.SerializeToString().c_str() <<
          " is not supported by this implementation.";
      return NOT_IMPLEMENTED;
    };
  };

  return STATUS_OK;
};


// Check that the bevy
AFF4Status AFF4Image::_FlushBevy() {
  // If the bevy is empty nothing else to do.
  if (bevy.Size() == 0) {
    LOG(INFO) << urn.SerializeToString() << "Bevy is empty.";
    return STATUS_OK;
  };

  URN bevy_urn(urn.Append(aff4_sprintf("%08d", bevy_number++)));
  URN bevy_index_urn(bevy_urn.Append("index"));

  // Open the volume.
  AFF4ScopedPtr<AFF4Volume> volume = resolver->AFF4FactoryOpen<AFF4Volume>(
      volume_urn);

  if (!volume) {
    return NOT_FOUND;
  };

  // Create the new segments in this zip file.
  AFF4ScopedPtr<AFF4Stream> bevy_index_stream = volume->CreateMember(bevy_index_urn);
  AFF4ScopedPtr<AFF4Stream> bevy_stream = volume->CreateMember(bevy_urn);

  if(!bevy_index_stream || ! bevy_stream) {
    LOG(ERROR) << "Unable to create bevy URN";
    return IO_ERROR;
  };

  bevy_index_stream->Write(bevy_index.buffer);
  bevy_stream->Write(bevy.buffer);

  // These calls flush the bevies and removes them from the resolver cache.
  resolver->Close(bevy_index_stream);
  resolver->Close(bevy_stream);

  bevy_index.Truncate();
  bevy.Truncate();

  chunk_count_in_bevy = 0;

  return STATUS_OK;
};


/**
 * Flush the current chunk into the current bevy.
 *
 * @param data: Chunk data. This should be a full chunk unless it is the last
 *        chunk in the stream which may be short.
 * @param length: Length of data.
 *
 * @return Status.
 */
AFF4Status AFF4Image::FlushChunk(const char *data, size_t length) {
  uint32_t bevy_offset = bevy.Tell();
  string output;

  AFF4Status result;

  switch(compression) {
    case AFF4_IMAGE_COMPRESSION_ENUM_ZLIB: {
      result = CompressZlib_(data, length, &output);
    } break;

    case AFF4_IMAGE_COMPRESSION_ENUM_SNAPPY: {
      result = CompressSnappy_(data, length, &output);
    } break;

    case AFF4_IMAGE_COMPRESSION_ENUM_STORED: {
      output.assign(data, length);
      result = STATUS_OK;
    } break;

    default:
      return IO_ERROR;
  };

  bevy_index.Write((char *)&bevy_offset, sizeof(bevy_offset));
  bevy.Write(output);

  chunk_count_in_bevy++;

  if(chunk_count_in_bevy >= chunks_per_segment) {
    return _FlushBevy();
  };

  return result;
};

AFF4Status AFF4Image::CompressZlib_(const char *data, size_t length,
                                      string *output) {
  uLongf c_length = compressBound(length) + 1;
  output->resize(c_length);

  if(compress2((Bytef *)output->data(), &c_length, (Bytef *)data, length,
               1) != Z_OK) {
    LOG(ERROR) << "Unable to compress chunk " << urn.SerializeToString();
    return MEMORY_ERROR;
  };

  output->resize(c_length);

  return STATUS_OK;
};

AFF4Status AFF4Image::DeCompressZlib_(const char *data, size_t length,
                                      string *buffer) {
  uLongf buffer_size = buffer->size();

  if(uncompress((Bytef *)buffer->data(), &buffer_size,
                (const Bytef *)data, length) == Z_OK) {

    buffer->resize(buffer_size);
    return STATUS_OK;
  };

  return IO_ERROR;
};


AFF4Status AFF4Image::CompressSnappy_(const char *data, size_t length,
                                        string *output) {
  snappy::Compress(data, length, output);

  return STATUS_OK;
};


AFF4Status AFF4Image::DeCompressSnappy_(const char *data, size_t length,
                                        string *output) {
  if(!snappy::Uncompress(data, length, output)) {
    return GENERIC_ERROR;
  };

  return STATUS_OK;
};


int AFF4Image::Write(const char *data, int length) {
  // This object is now dirty.
  MarkDirty();

  buffer.append(data, length);
  size_t offset = 0;
  const char *chunk_ptr = buffer.data();

  // Consume full chunks from the buffer.
  while (buffer.length() - offset >= chunk_size) {
    if(FlushChunk(chunk_ptr + offset, chunk_size) != STATUS_OK)
      return 0;

    offset += chunk_size;
  };

  // Keep the last part of the buffer which is smaller than a chunk size.
  buffer.erase(0, offset);

  readptr += length;
  if (readptr > size) {
    size = readptr;
  };

  return length;
};


/**
 * Read a single chunk from the bevy and append it to result.
 *
 * @param result: A string which will receive the chunk data.
 * @param bevy: The bevy to read from.
 * @param bevy_index: A bevy index array - the is the offset of each chunk in
 *        the bevy.
 * @param index_size: The length of the bevy index array.
 *
 * @return number of bytes read, or AFF4Status for error.
 */
AFF4Status AFF4Image::_ReadChunkFromBevy(
    string &result, unsigned int chunk_id, AFF4ScopedPtr<AFF4Stream> &bevy,
    uint32_t bevy_index[], uint32_t index_size) {

  unsigned int chunk_id_in_bevy = chunk_id % chunks_per_segment;
  unsigned int compressed_chunk_size;

  if (index_size == 0) {
    LOG(ERROR) << "Index empty in " <<
        urn.SerializeToString() << ":" << chunk_id;
    return IO_ERROR;
  };

  // The segment is not completely full.
  if (chunk_id_in_bevy >= index_size) {
    LOG(ERROR) << "Bevy index too short in " <<
        urn.SerializeToString() << ":" << chunk_id;
    return IO_ERROR;

    // For the last chunk in the bevy, consume to the end of the bevy segment.
  } else if (chunk_id_in_bevy == index_size - 1) {
    compressed_chunk_size = bevy->Size() - bevy->Tell();

  } else {
    compressed_chunk_size = (bevy_index[chunk_id_in_bevy + 1] -
                             bevy_index[chunk_id_in_bevy]);
  };


  bevy->Seek(bevy_index[chunk_id_in_bevy], SEEK_SET);
  string cbuffer = bevy->Read(compressed_chunk_size);

  string buffer;
  buffer.resize(chunk_size);

  AFF4Status res;

  switch(compression) {
    case AFF4_IMAGE_COMPRESSION_ENUM_ZLIB: {
      res = DeCompressZlib_(cbuffer.data(), cbuffer.length(),
                            &buffer);
    }; break;

    case AFF4_IMAGE_COMPRESSION_ENUM_SNAPPY: {
      res = DeCompressSnappy_(cbuffer.data(), cbuffer.length(),
                              &buffer);
    }; break;

    case AFF4_IMAGE_COMPRESSION_ENUM_STORED: {
      buffer = cbuffer;
      res = STATUS_OK;
    }; break;

      // Should never happen because the object should never accept this
      // compression URN.
    default:
      LOG(FATAL) << "Unexpected compression type set";
  };

  if(res != STATUS_OK) {
    LOG(ERROR) << urn.SerializeToString() <<
        ": Unable to uncompress chunk " << chunk_id;
    return res;
  };

  result += buffer;
  return STATUS_OK;
};

int AFF4Image::_ReadPartial(unsigned int chunk_id, int chunks_to_read,
                            string &result) {
  int chunks_read = 0;

  while (chunks_to_read > 0) {
    unsigned int bevy_id = chunk_id / chunks_per_segment;
    URN bevy_urn = urn.Append(aff4_sprintf("%08d", bevy_id));
    URN bevy_index_urn = bevy_urn.Append("index");

    AFF4ScopedPtr<AFF4Stream> bevy_index = resolver->AFF4FactoryOpen<AFF4Stream>(
        bevy_index_urn);

    AFF4ScopedPtr<AFF4Stream> bevy = resolver->AFF4FactoryOpen<AFF4Stream>(
        bevy_urn);

    if(!bevy_index || !bevy) {
      LOG(ERROR) << "Unable to open bevy " <<
          bevy_urn.SerializeToString();
      return -1;
    }

    uint32_t index_size = bevy_index->Size() / sizeof(uint32_t);
    string bevy_index_data = bevy_index->Read(bevy_index->Size());

    uint32_t *bevy_index_array = (uint32_t *)bevy_index_data.data();

    while (chunks_to_read > 0) {
      // Read a full chunk from the bevy.
      AFF4Status res = _ReadChunkFromBevy(
          result, chunk_id, bevy, bevy_index_array, index_size);

      if(res != STATUS_OK) {
        return res;
      };

      chunks_to_read--;
      chunk_id++;
      chunks_read++;

      // This bevy is exhausted, get the next one.
      if(bevy_id < chunk_id / chunks_per_segment) {
        break;
      }
    };
  };

  return chunks_read;
};

string AFF4Image::Read(size_t length) {
  if(length > AFF4_MAX_READ_LEN)
    return "";

  length = std::min((aff4_off_t)length, Size() - readptr);

  int initial_chunk_offset = readptr % chunk_size;
  // We read this many full chunks at once.
  int chunks_to_read = length / chunk_size + 1;
  unsigned int chunk_id = readptr / chunk_size;
  string result;

  // Make sure we have enough room for output.
  result.reserve(chunks_to_read * chunk_size);

  while(chunks_to_read > 0) {
    int chunks_read = _ReadPartial(chunk_id, chunks_to_read, result);
    // Error occured.
    if (chunks_read < 0) {
      return "";
    } else if (chunks_read == 0) {
      break;
    };

    chunks_to_read -= chunks_read;
  };

  if (initial_chunk_offset) {
    result.erase(0, initial_chunk_offset);
  };

  result.resize(length);
  readptr += length;

  return result;
};


AFF4Status AFF4Image::Flush() {
  if(IsDirty()) {
    // Flush the last chunk.
    FlushChunk(buffer.c_str(), buffer.length());
    buffer.resize(0);
    _FlushBevy();

    resolver->Set(urn, AFF4_TYPE, new URN(AFF4_IMAGE_TYPE));

    resolver->Set(urn, AFF4_STORED, new URN(volume_urn));

    resolver->Set(urn, AFF4_IMAGE_CHUNK_SIZE,
                  new XSDInteger(chunk_size));

    resolver->Set(urn, AFF4_IMAGE_CHUNKS_PER_SEGMENT,
                  new XSDInteger(chunks_per_segment));

    resolver->Set(urn, AFF4_STREAM_SIZE, new XSDInteger(size));

    resolver->Set(urn, AFF4_IMAGE_COMPRESSION, new URN(
        CompressionMethodToURN(compression)));
  };

  // Always call the baseclass to ensure the object is marked non dirty.
  return AFF4Stream::Flush();
};

static AFF4Registrar<AFF4Image> r1(AFF4_IMAGE_TYPE);
