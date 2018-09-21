#ifndef PACKET_H
#define PACKET_H

#include <cstdint>
#include <vector>

#include "Common.h"


/// Packet types (in a compressed vector section)
#define E57_INDEX_PACKET                0
#define E57_DATA_PACKET                 1
#define E57_EMPTY_PACKET                2

#define E57_DATA_PACKET_MAX (64*1024)  /// maximum size of CompressedVector binary data packet   ??? where put this

namespace e57 {
   class CheckedFile;
   class PacketLock;

   class PacketReadCache {
      public:
         PacketReadCache(CheckedFile* cFile, unsigned packetCount);
         ~PacketReadCache();

         std::unique_ptr<PacketLock> lock(uint64_t packetLogicalOffset, char* &pkt);  //??? pkt could be const

#ifdef E57_DEBUG
         void                dump(int indent = 0, std::ostream& os = std::cout);
#endif
      protected:
         /// Only PacketLock can unlock the cache
         friend class PacketLock;
         void                unlock(unsigned cacheIndex);

         void                readPacket(unsigned oldestEntry, uint64_t packetLogicalOffset);

         struct CacheEntry {
               uint64_t    logicalOffset_;
               char*       buffer_;  //??? could be const?
               unsigned    lastUsed_;
         };

         unsigned            lockCount_;
         unsigned            useCount_;
         CheckedFile*        cFile_;
         std::vector<CacheEntry>  entries_;
   };

   class PacketLock {
      public:
         ~PacketLock();

      private:
         /// Can't be copied or assigned
         PacketLock(const PacketLock& plock);
         PacketLock&     operator=(const PacketLock& plock);

      protected:
         friend class PacketReadCache;
         /// Only PacketReadCache can construct
         PacketLock(PacketReadCache* cache, unsigned cacheIndex);

         PacketReadCache* cache_;
         unsigned         cacheIndex_;
   };

   struct DataPacketHeader {  ///??? where put this
         uint8_t     packetType;         // = E57_DATA_PACKET
         uint8_t     packetFlags;
         uint16_t    packetLogicalLengthMinus1;
         uint16_t    bytestreamCount;

         DataPacketHeader();
         void        verify(unsigned bufferLength = 0) const; //???use
#ifdef E57_BIGENDIAN
         void        swab();
#else
         void        swab(){}
#endif
#ifdef E57_DEBUG
         void        dump(int indent = 0, std::ostream& os = std::cout) const;
#endif
   };

   struct DataPacket {  /// Note this is full sized packet, not just header
         uint8_t     packetType;         // = E57_DATA_PACKET
         uint8_t     packetFlags;
         uint16_t    packetLogicalLengthMinus1;
         uint16_t    bytestreamCount;
         uint8_t     payload[64*1024-6]; // pad packet to full length, can't spec layout because depends bytestream data

         DataPacket();
         void        verify(unsigned bufferLength = 0) const;
         char*       getBytestream(unsigned bytestreamNumber, unsigned& bufferLength);
         unsigned    getBytestreamBufferLength(unsigned bytestreamNumber);

#ifdef E57_BIGENDIAN
         void        swab(bool toLittleEndian);    //??? change to swabIfBigEndian() and elsewhere
#else
         void        swab(bool /*toLittleEndian*/){}
#endif
#ifdef E57_DEBUG
         void        dump(int indent = 0, std::ostream& os = std::cout) const;
#endif
   };
}
#endif
