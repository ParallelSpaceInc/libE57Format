#include "Packet.h"
#include "CheckedFile.h"


namespace e57 {
   struct IndexPacket {  /// Note this is whole packet, not just header
         static const unsigned MAX_ENTRIES = 2048;

         uint8_t     packetType;     // = E57_INDEX_PACKET
         uint8_t     packetFlags;    // flag bitfields
         uint16_t    packetLogicalLengthMinus1;
         uint16_t    entryCount;
         uint8_t     indexLevel;
         uint8_t     reserved1[9];   // must be zero

         struct IndexPacketEntry {
               uint64_t    chunkRecordNumber;
               uint64_t    chunkPhysicalOffset;
         } entries[MAX_ENTRIES];

         IndexPacket();
         void        verify(unsigned bufferLength = 0, uint64_t totalRecordCount = 0, uint64_t fileSize = 0) const;
#ifdef E57_BIGENDIAN
         void        swab(bool toLittleEndian);
#else
         void        swab(bool /*toLittleEndian*/) {}
#endif
#ifdef E57_DEBUG
         void        dump(int indent = 0, std::ostream& os = std::cout) const;
#endif
   };

   struct EmptyPacketHeader {
         uint8_t     packetType;    // = E57_EMPTY_PACKET
         uint8_t     reserved1;     // must be zero
         uint16_t    packetLogicalLengthMinus1;

         EmptyPacketHeader();
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
}

namespace e57 {

   //=============================================================================
   // PacketReadCache

   PacketReadCache::PacketReadCache(CheckedFile* cFile, unsigned packetCount)
      : lockCount_(0),
        useCount_(0),
        cFile_(cFile),
        entries_(packetCount)
   {
      if (packetCount == 0)
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL, "packetCount=" + toString(packetCount));

      /// Allocate requested number of maximum sized data packets buffers for holding data read from file
      for (unsigned i=0; i < entries_.size(); i++) {
         entries_.at(i).logicalOffset_ = 0;
         entries_.at(i).buffer_        = new char[E57_DATA_PACKET_MAX];
         entries_.at(i).lastUsed_      = 0;
      }
   }

   PacketReadCache::~PacketReadCache()
   {
      /// Free allocated packet buffers
      for (unsigned i=0; i < entries_.size(); i++) {
         delete [] entries_.at(i).buffer_;
         entries_.at(i).buffer_ = nullptr;
      }
   }

   std::unique_ptr<PacketLock> PacketReadCache::lock(uint64_t packetLogicalOffset, char* &pkt)
   {
#ifdef E57_MAX_VERBOSE
      cout << "PacketReadCache::lock() called, packetLogicalOffset=" << packetLogicalOffset << endl;
#endif

      /// Only allow one locked packet at a time.
      if (lockCount_ > 0)
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL, "lockCount=" + toString(lockCount_));

      /// Offset can't be 0
      if (packetLogicalOffset == 0)
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL, "packetLogicalOffset=" + toString(packetLogicalOffset));

      /// Linear scan for matching packet offset in cache
      for (unsigned i = 0; i < entries_.size(); i++)
      {
         if (packetLogicalOffset == entries_[i].logicalOffset_)
         {
            /// Found a match, so don't have to read anything
#ifdef E57_MAX_VERBOSE
            cout << "  Found matching cache entry, index=" << i << endl;
#endif
            /// Mark entry with current useCount (keeps track of age of entry).
            entries_[i].lastUsed_ = ++useCount_;

            /// Publish buffer address to caller
            pkt = entries_[i].buffer_;

            /// Create lock so we are sure that we will be unlocked when use is finished.
            std::unique_ptr<PacketLock> plock(new PacketLock(this, i));

            /// Increment cache lock just before return
            lockCount_++;

            return plock;
         }
      }
      /// Get here if didn't find a match already in cache.

      /// Find least recently used (LRU) packet buffer
      unsigned oldestEntry = 0;
      unsigned oldestUsed = entries_.at(0).lastUsed_;

      for (unsigned i = 0; i < entries_.size(); i++)
      {
         if (entries_[i].lastUsed_ < oldestUsed)
         {
            oldestEntry = i;
            oldestUsed = entries_[i].lastUsed_;
         }
      }
#ifdef E57_MAX_VERBOSE
      cout << "  Oldest entry=" << oldestEntry << " lastUsed=" << oldestUsed << endl;
#endif

      readPacket(oldestEntry, packetLogicalOffset);

      /// Publish buffer address to caller
      pkt = entries_[oldestEntry].buffer_;

      /// Create lock so we are sure we will be unlocked when use is finished.
      std::unique_ptr<PacketLock> plock(new PacketLock(this, oldestEntry));

      /// Increment cache lock just before return
      lockCount_++;

      return plock;
   }

   void PacketReadCache::unlock(unsigned lockedEntry)
   {
      //??? why lockedEntry not used?
#ifdef E57_MAX_VERBOSE
      cout << "PacketReadCache::unlock() called, lockedEntry=" << lockedEntry << endl;
#endif

      if (lockCount_ != 1)
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL, "lockCount=" + toString(lockCount_));

      lockCount_--;
   }

   void PacketReadCache::readPacket(unsigned oldestEntry, uint64_t packetLogicalOffset)
   {
#ifdef E57_MAX_VERBOSE
      cout << "PacketReadCache::readPacket() called, oldestEntry=" << oldestEntry << " packetLogicalOffset=" << packetLogicalOffset << endl;
#endif

      /// Read header of packet first to get length.  Use EmptyPacketHeader since it has the commom fields to all packets.
      EmptyPacketHeader header;
      cFile_->seek(packetLogicalOffset, CheckedFile::Logical);
      cFile_->read(reinterpret_cast<char*>(&header), sizeof(header));
      header.swab();
      /// Can't verify packet header here, because it is not really an EmptyPacketHeader.
      unsigned packetLength = header.packetLogicalLengthMinus1+1;

      /// Be paranoid about packetLength before read
      if (packetLength > E57_DATA_PACKET_MAX)
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "packetLength=" + toString(packetLength));

      /// Now read in whole packet into preallocated buffer_.  Note buffer is
      cFile_->seek(packetLogicalOffset, CheckedFile::Logical);
      cFile_->read(entries_.at(oldestEntry).buffer_, packetLength);

      /// Swab if necessary, then verify that packet is good.
      switch (header.packetType)
      {
         case E57_DATA_PACKET: {
            DataPacket* dpkt = reinterpret_cast<DataPacket*>(entries_.at(oldestEntry).buffer_);
#ifdef E57_BIGENDIAN
            dpkt->swab(false);
#endif
            dpkt->verify(packetLength);
#ifdef E57_MAX_VERBOSE
            cout << "  data packet:" << endl;
            dpkt->dump(4); //???
#endif
         }
            break;
         case E57_INDEX_PACKET: {
            IndexPacket* ipkt = reinterpret_cast<IndexPacket*>(entries_.at(oldestEntry).buffer_);
#ifdef E57_BIGENDIAN
            ipkt->swab(false);
#endif
            ipkt->verify(packetLength);
#ifdef E57_MAX_VERBOSE
            cout << "  index packet:" << endl;
            ipkt->dump(4); //???
#endif
         }
            break;
         case E57_EMPTY_PACKET: {
            EmptyPacketHeader* hp = reinterpret_cast<EmptyPacketHeader*>(entries_.at(oldestEntry).buffer_);
            hp->swab();
            hp->verify(packetLength);
#ifdef E57_MAX_VERBOSE
            cout << "  empty packet:" << endl;
            hp->dump(4); //???
#endif
         }
            break;
         default:
            throw E57_EXCEPTION2(E57_ERROR_INTERNAL, "packetType=" + toString(header.packetType));
      }

      entries_[oldestEntry].logicalOffset_ = packetLogicalOffset;

      /// Mark entry with current useCount (keeps track of age of entry).
      /// This is a cache, so a small hiccup when useCount_ overflows won't hurt.
      entries_[oldestEntry].lastUsed_ = ++useCount_;
   }

#ifdef E57_DEBUG
   void PacketReadCache::dump(int indent, std::ostream& os)
   {
      os << space(indent) << "lockCount: " << lockCount_ << std::endl;
      os << space(indent) << "useCount:  " << useCount_ << std::endl;
      os << space(indent) << "entries:" << std::endl;
      for (unsigned i=0; i < entries_.size(); i++) {
         os << space(indent) << "entry[" << i << "]:" << std::endl;
         os << space(indent+4) << "logicalOffset:  " << entries_[i].logicalOffset_ << std::endl;
         os << space(indent+4) << "lastUsed:        " << entries_[i].lastUsed_ << std::endl;
         if (entries_[i].logicalOffset_ != 0) {
            os << space(indent+4) << "packet:" << std::endl;
            switch (reinterpret_cast<EmptyPacketHeader*>(entries_.at(i).buffer_)->packetType) {
               case E57_DATA_PACKET: {
                  DataPacket* dpkt = reinterpret_cast<DataPacket*>(entries_.at(i).buffer_);
                  dpkt->dump(indent+6, os);
               }
                  break;
               case E57_INDEX_PACKET: {
                  IndexPacket* ipkt = reinterpret_cast<IndexPacket*>(entries_.at(i).buffer_);
                  ipkt->dump(indent+6, os);
               }
                  break;
               case E57_EMPTY_PACKET: {
                  EmptyPacketHeader* hp = reinterpret_cast<EmptyPacketHeader*>(entries_.at(i).buffer_);
                  hp->dump(indent+6, os);
               }
                  break;
               default:
                  throw E57_EXCEPTION2(E57_ERROR_INTERNAL,
                                       "packetType=" + toString(reinterpret_cast<EmptyPacketHeader*>(entries_.at(i).buffer_)->packetType));
            }
         }
      }
   }
#endif

   //=============================================================================
   // PacketLock

   PacketLock::PacketLock(PacketReadCache* cache, unsigned cacheIndex)
      : cache_(cache),
        cacheIndex_(cacheIndex)
   {
#ifdef E57_MAX_VERBOSE
      cout << "PacketLock() called" << endl;
#endif
   }

   PacketLock::~PacketLock()
   {
#ifdef E57_MAX_VERBOSE
      cout << "~PacketLock() called" << endl;
#endif
      try {
         /// Note cache must live longer than lock, this is reasonable assumption.
         cache_->unlock(cacheIndex_);
      } catch (...) {
         //??? report?
      }
   }

   //=============================================================================
   // DataPacketHeader

   DataPacketHeader::DataPacketHeader()
   {
      /// Double check that packet struct is correct length.  Watch out for RTTI increasing the size.
      if (sizeof(*this) != 6)
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL, "size=" + toString(sizeof(*this)));

      /// Now confident we have correct size, zero packet.
      /// This guarantees that data packet headers are always completely initialized to zero.
      memset(this, 0, sizeof(*this));
   }

   void DataPacketHeader::verify(unsigned bufferLength) const
   {
      /// Verify that packet is correct type
      if (packetType != E57_DATA_PACKET)
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "packetType=" + toString(packetType));

      /// ??? check reserved flags zero?

      /// Check packetLength is at least large enough to hold header
      unsigned packetLength = packetLogicalLengthMinus1+1;
      if (packetLength < sizeof(*this))
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "packetLength=" + toString(packetLength));

      /// Check packet length is multiple of 4
      if (packetLength % 4)
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "packetLength=" + toString(packetLength));

      /// Check actual packet length is large enough.
      if (bufferLength > 0 && packetLength > bufferLength) {
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET,
                              "packetLength=" + toString(packetLength)
                              + " bufferLength=" + toString(bufferLength));
      }

      /// Make sure there is at least one entry in packet  ??? 0 record cvect allowed?
      if (bytestreamCount == 0)
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "bytestreamCount=" + toString(bytestreamCount));

      /// Check packet is at least long enough to hold bytestreamBufferLength array
      if (sizeof(DataPacketHeader) + 2*bytestreamCount > packetLength) {
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET,
                              "packetLength=" + toString(packetLength)
                              + " bytestreamCount=" + toString(bytestreamCount));
      }
   }

#ifdef E57_BIGENDIAN
   void DataPacketHeader::swab()
   {
      /// Byte swap fields in-place, if CPU is BIG_ENDIAN
      swab(&packetLogicalLengthMinus1);
      swab(&bytestreamCount);
   };
#endif

#ifdef E57_DEBUG
   void DataPacketHeader::dump(int indent, std::ostream& os) const
   {
      os << space(indent) << "packetType:                " << static_cast<unsigned>(packetType) << std::endl;
      os << space(indent) << "packetFlags:               " << static_cast<unsigned>(packetFlags) << std::endl;
      os << space(indent) << "packetLogicalLengthMinus1: " << packetLogicalLengthMinus1 << std::endl;
      os << space(indent) << "bytestreamCount:           " << bytestreamCount << std::endl;
   }
#endif

   //=============================================================================
   // DataPacket

   DataPacket::DataPacket()
   {
      /// Double check that packet struct is correct length.  Watch out for RTTI increasing the size.
      if (sizeof(*this) != 64*1024)
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL, "size=" + toString(sizeof(*this)));

      /// Now confident we have correct size, zero packet.
      /// This guarantees that data packets are always completely initialized to zero.
      memset(this, 0, sizeof(*this));
   }

   void DataPacket::verify(unsigned bufferLength) const
   {
      //??? do all packets need versions?  how extend without breaking older checking?  need to check file version#?

      /// Verify header is good
      const DataPacketHeader* hp = reinterpret_cast<const DataPacketHeader*>(this);

      hp->verify(bufferLength);

      /// Calc sum of lengths of each bytestream buffer in this packet
      const uint16_t* bsbLength = reinterpret_cast<const uint16_t*>(&payload[0]);
      unsigned totalStreamByteCount = 0;

      for (unsigned i=0; i < bytestreamCount; i++)
      {
         totalStreamByteCount += bsbLength[i];
      }

      /// Calc size of packet needed
      const unsigned packetLength = packetLogicalLengthMinus1+1;
      const unsigned needed = sizeof(DataPacketHeader) + 2*bytestreamCount + totalStreamByteCount;
#ifdef E57_MAX_VERBOSE
      cout << "needed=" << needed << " actual=" << packetLength << endl; //???
#endif

      /// If needed is not with 3 bytes of actual packet size, have an error
      if (needed > packetLength || needed+3 < packetLength)
      {
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET,
                              "needed=" + toString(needed)
                              + "packetLength=" + toString(packetLength));
      }

      /// Verify that padding at end of packet is zero
      for (unsigned i=needed; i < packetLength; i++)
      {
         if (reinterpret_cast<const char*>(this)[i] != 0)
         {
            throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "i=" + toString(i));
         }
      }
   }

   char* DataPacket::getBytestream(unsigned bytestreamNumber, unsigned& byteCount)
   {
#ifdef E57_MAX_VERBOSE
      cout << "getBytestream called, bytestreamNumber=" << bytestreamNumber << endl;
#endif

      /// Verify that packet is correct type
      if (packetType != E57_DATA_PACKET)
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "packetType=" + toString(packetType));

      /// Check bytestreamNumber in bounds
      if (bytestreamNumber >= bytestreamCount) {
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL,
                              "bytestreamNumber=" + toString(bytestreamNumber)
                              + "bytestreamCount=" + toString(bytestreamCount));
      }

      /// Calc positions in packet
      uint16_t* bsbLength = reinterpret_cast<uint16_t*>(&payload[0]);
      char* streamBase = reinterpret_cast<char*>(&bsbLength[bytestreamCount]);

      /// Sum size of preceeding stream buffers to get position
      unsigned totalPreceeding = 0;
      for (unsigned i=0; i < bytestreamNumber; i++)
         totalPreceeding += bsbLength[i];

      byteCount = bsbLength[bytestreamNumber];

      /// Double check buffer is completely within packet
      if (sizeof(DataPacketHeader) + 2*bytestreamCount + totalPreceeding + byteCount > packetLogicalLengthMinus1 + 1U) {
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL,
                              "bytestreamCount=" + toString(bytestreamCount)
                              + " totalPreceeding=" + toString(totalPreceeding)
                              + " byteCount=" + toString(byteCount)
                              + " packetLogicalLengthMinus1=" + toString(packetLogicalLengthMinus1));
      }

      /// Return start of buffer
      return(&streamBase[totalPreceeding]);
   }

   unsigned DataPacket::getBytestreamBufferLength(unsigned bytestreamNumber)
   {
      //??? for now:
      unsigned byteCount;
      (void) getBytestream(bytestreamNumber, byteCount);
      return(byteCount);
   }

#ifdef E57_BIGENDIAN
   DataPacket::swab(bool toLittleEndian)
   {
      /// Be a little paranoid
      if (packetType != E57_INDEX_PACKET)
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL, "packetType=" + toString(packetType));

      swab(packetLogicalLengthMinus1);

      /// Need to watch out if packet starts out in natural CPU ordering or not
      unsigned goodEntryCount;
      if (toLittleEndian) {
         /// entryCount starts out in correct order, save it before trashing
         goodEntryCount = entryCount;
         swab(entryCount);
      } else {
         /// Have to fix entryCount before can use.
         swab(entryCount);
         goodEntryCount = entryCount;
      }

      /// Make sure we wont go off end of buffer (e.g. if we accidentally swab)
      if (goodEntryCount > MAX_ENTRIES)
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "goodEntryCount=" + toString(goodEntryCount));

      for (unsigned i=0; i < goodEntryCount; i++) {
         swab(entries[i].chunkRecordNumber);
         swab(entries[i].chunkPhysicalOffset);
      }
   }
#endif

#ifdef E57_DEBUG
   void DataPacket::dump(int indent, std::ostream& os) const
   {
      if (packetType != E57_DATA_PACKET)
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL, "packetType=" + toString(packetType));
      reinterpret_cast<const DataPacketHeader*>(this)->dump(indent, os);

      const uint16_t* bsbLength = reinterpret_cast<const uint16_t*>(&payload[0]);
      const uint8_t* p = reinterpret_cast<const uint8_t*>(&bsbLength[bytestreamCount]);

      for (unsigned i=0; i < bytestreamCount; i++)
      {
         os << space(indent) << "bytestream[" << i << "]:" << std::endl;
         os << space(indent+4) << "length: " << bsbLength[i] << std::endl;
         /*====
           unsigned j;
           for (j=0; j < bsbLength[i] && j < 10; j++)
               os << space(indent+4) << "byte[" << j << "]=" << (unsigned)p[j] << endl;
           if (j < bsbLength[i])
               os << space(indent+4) << bsbLength[i]-j << " more unprinted..." << endl;
   ====*/
         p += bsbLength[i];
         if (p - reinterpret_cast<const uint8_t*>(this) > E57_DATA_PACKET_MAX)
            throw E57_EXCEPTION2(E57_ERROR_INTERNAL, "size=" + toString(p - reinterpret_cast<const uint8_t*>(this)));
      }
   }
#endif

   //=============================================================================
   // IndexPacket

   IndexPacket::IndexPacket()
   {
      /// Double check that packet struct is correct length.  Watch out for RTTI increasing the size.
      if (sizeof(*this) != 16+16*MAX_ENTRIES)
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL, "size=" + toString(sizeof(*this)));

      /// Now confident we have correct size, zero packet.
      /// This guarantees that index packets are always completely initialized to zero.
      memset(this, 0, sizeof(*this));
   }

   void IndexPacket::verify(unsigned bufferLength, uint64_t totalRecordCount, uint64_t fileSize) const
   {
      //??? do all packets need versions?  how extend without breaking older checking?  need to check file version#?

      /// Verify that packet is correct type
      if (packetType != E57_INDEX_PACKET)
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "packetType=" + toString(packetType));

      /// Check packetLength is at least large enough to hold header
      unsigned packetLength = packetLogicalLengthMinus1+1;
      if (packetLength < sizeof(*this))
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "packetLength=" + toString(packetLength));

      /// Check packet length is multiple of 4
      if (packetLength % 4)
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "packetLength=" + toString(packetLength));

      /// Make sure there is at least one entry in packet  ??? 0 record cvect allowed?
      if (entryCount == 0)
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "entryCount=" + toString(entryCount));

      /// Have to have <= 2048 entries
      if (entryCount > MAX_ENTRIES)
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "entryCount=" + toString(entryCount));

      /// Index level should be <= 5.  Because (5+1)* 11 bits = 66 bits, which will cover largest number of chunks possible.
      if (indexLevel > 5)
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "indexLevel=" + toString(indexLevel));

      /// Index packets above level 0 must have at least two entries (otherwise no point to existing).
      ///??? check that this is in spec
      if (indexLevel > 0 && entryCount < 2)
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "indexLevel=" + toString(indexLevel) + " entryCount=" + toString(entryCount));

      /// If not later version, verify reserved fields are zero. ??? test file version
      /// if (version <= E57_FORMAT_MAJOR) { //???
      for (unsigned i=0; i < sizeof(reserved1); i++) {
         if (reserved1[i] != 0)
            throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "i=" + toString(i));
      }

      /// Check actual packet length is large enough.
      if (bufferLength > 0 && packetLength > bufferLength) {
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET,
                              "packetLength=" + toString(packetLength)
                              + " bufferLength=" + toString(bufferLength));
      }

      /// Check if entries will fit in space provided
      unsigned neededLength = 16 + 8*entryCount;
      if (packetLength < neededLength) {
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET,
                              "packetLength=" + toString(packetLength)
                              + " neededLength=" + toString(neededLength));
      }

#ifdef E57_MAX_DEBUG
      /// Verify padding at end is zero.
      const char* p = reinterpret_cast<const char*>(this);
      for (unsigned i=neededLength; i < packetLength; i++) {
         if (p[i] != 0)
            throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "i=" + toString(i));
      }

      /// Verify records and offsets are in sorted order
      for (unsigned i=0; i < entryCount; i++) {
         /// Check chunkRecordNumber is in bounds
         if (totalRecordCount > 0 && entries[i].chunkRecordNumber >= totalRecordCount) {
            throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET,
                                 "i=" + toString(i)
                                 + " chunkRecordNumber=" + toString(entries[i].chunkRecordNumber)
                                 + " totalRecordCount=" + toString(totalRecordCount));
         }

         /// Check record numbers are strictly increasing
         if (i > 0 && entries[i-1].chunkRecordNumber >= entries[i].chunkRecordNumber) {
            throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET,
                                 "i=" + toString(i)
                                 + " prevChunkRecordNumber=" + toString(entries[i-1].chunkRecordNumber)
                  + " currentChunkRecordNumber=" + toString(entries[i].chunkRecordNumber));
         }

         /// Check chunkPhysicalOffset is in bounds
         if (fileSize > 0 && entries[i].chunkPhysicalOffset >= fileSize) {
            throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET,
                                 "i=" + toString(i)
                                 + " chunkPhysicalOffset=" + toString(entries[i].chunkPhysicalOffset)
                                 + " fileSize=" + toString(fileSize));
         }

         /// Check chunk offsets are strictly increasing
         if (i > 0 && entries[i-1].chunkPhysicalOffset >= entries[i].chunkPhysicalOffset) {
            throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET,
                                 "i=" + toString(i)
                                 + " prevChunkPhysicalOffset=" + toString(entries[i-1].chunkPhysicalOffset)
                  + " currentChunkPhysicalOffset=" + toString(entries[i].chunkPhysicalOffset));
         }
      }
#endif
   }

#ifdef E57_BIGENDIAN
   IndexPacket::swab(bool toLittleEndian)
   {
      /// Be a little paranoid
      if (packetType != E57_INDEX_PACKET)
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL, "packetType=" + toString(packetType));

      swab(packetLogicalLengthMinus1);

      /// Need to watch out if packet starts out in natural CPU ordering or not
      unsigned goodEntryCount;
      if (toLittleEndian) {
         /// entryCount starts out in correct order, save it before trashing
         goodEntryCount = entryCount;
         swab(entryCount);
      } else {
         /// Have to fix entryCount before can use.
         swab(entryCount);
         goodEntryCount = entryCount;
      }

      /// Make sure we wont go off end of buffer (e.g. if we accidentally swab)
      if (goodEntryCount > MAX_ENTRIES)
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "goodEntryCount=" + toString(goodEntryCount));

      for (unsigned i=0; i < goodEntryCount; i++) {
         swab(entries[i].chunkRecordNumber);
         swab(entries[i].chunkPhysicalOffset);
      }
   }
#endif

#ifdef E57_DEBUG
   void IndexPacket::dump(int indent, std::ostream& os) const
   {
      os << space(indent) << "packetType:                " << static_cast<unsigned>(packetType) << std::endl;
      os << space(indent) << "packetFlags:               " << static_cast<unsigned>(packetFlags) << std::endl;
      os << space(indent) << "packetLogicalLengthMinus1: " << packetLogicalLengthMinus1 << std::endl;
      os << space(indent) << "entryCount:                " << entryCount << std::endl;
      os << space(indent) << "indexLevel:                " << indexLevel << std::endl;
      unsigned i;
      for (i=0; i < entryCount && i < 10; i++) {
         os << space(indent) << "entry[" << i << "]:" << std::endl;
         os << space(indent+4) << "chunkRecordNumber:    " << entries[i].chunkRecordNumber << std::endl;
         os << space(indent+4) << "chunkPhysicalOffset:  " << entries[i].chunkPhysicalOffset << std::endl;
      }
      if (i < entryCount)
         os << space(indent) << entryCount-i << "more entries unprinted..." << std::endl;
   }
#endif

   //=============================================================================
   // EmptyPacketHeader

   EmptyPacketHeader::EmptyPacketHeader()
   {
      /// Double check that packet struct is correct length.  Watch out for RTTI increasing the size.
      if (sizeof(*this) != 4)
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL, "size=" + toString(sizeof(*this)));

      /// Now confident we have correct size, zero packet.
      /// This guarantees that EmptyPacket headers are always completely initialized to zero.
      memset(this, 0, sizeof(*this));
   }

   void EmptyPacketHeader::verify(unsigned bufferLength) const
   {
      /// Verify that packet is correct type
      if (packetType != E57_EMPTY_PACKET)
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "packetType=" + toString(packetType));

      /// Check packetLength is at least large enough to hold header
      unsigned packetLength = packetLogicalLengthMinus1+1;
      if (packetLength < sizeof(*this))
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "packetLength=" + toString(packetLength));

      /// Check packet length is multiple of 4
      if (packetLength % 4)
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET, "packetLength=" + toString(packetLength));

      /// Check actual packet length is large enough.
      if (bufferLength > 0 && packetLength > bufferLength) {
         throw E57_EXCEPTION2(E57_ERROR_BAD_CV_PACKET,
                              "packetLength=" + toString(packetLength)
                              + " bufferLength=" + toString(bufferLength));
      }
   }

#ifdef E57_BIGENDIAN
   void EmptyPacketHeader::swab()
   {
      /// Byte swap fields in-place, if CPU is BIG_ENDIAN
      SWAB(&packetLogicalLengthMinus1);
   };
#endif

#ifdef E57_DEBUG
   void EmptyPacketHeader::dump(int indent, std::ostream& os) const
   {
      os << space(indent) << "packetType:                " << static_cast<unsigned>(packetType) << std::endl;
      os << space(indent) << "packetLogicalLengthMinus1: " << packetLogicalLengthMinus1 << std::endl;
   }
#endif
}
