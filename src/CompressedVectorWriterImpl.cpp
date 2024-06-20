/*
 * Original work Copyright 2009 - 2010 Kevin Ackley (kackley@gwi.net)
 * Modified work Copyright 2018 - 2020 Andy Maloney <asmaloney@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person or organization
 * obtaining a copy of the software and accompanying documentation covered by
 * this license (the "Software") to use, reproduce, display, distribute,
 * execute, and transmit the Software, and to prepare derivative works of the
 * Software, and to permit third-parties to whom the Software is furnished to
 * do so, all subject to the following:
 *
 * The copyright notices in the Software and this entire statement, including
 * the above license grant, this restriction and the following disclaimer,
 * must be included in all copies of the Software, in whole or in part, and
 * all derivative works of the Software, unless such copies or derivative
 * works are solely in the form of machine-executable object code generated by
 * a source language processor.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
 * SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
 * FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <cmath>
#include <numeric>

#include "CheckedFile.h"
#include "CompressedVectorNodeImpl.h"
#include "CompressedVectorWriterImpl.h"
#include "ImageFileImpl.h"
#include "SectionHeaders.h"
#include "SourceDestBufferImpl.h"
#include "StringFunctions.h"

namespace e57
{
   struct SortByBytestreamNumber
   {
      bool operator()( const std::shared_ptr<Encoder> &lhs,
                       const std::shared_ptr<Encoder> &rhs ) const
      {
         return ( lhs->bytestreamNumber() < rhs->bytestreamNumber() );
      }
   };

   CompressedVectorWriterImpl::CompressedVectorWriterImpl(
      std::shared_ptr<CompressedVectorNodeImpl> ni, std::vector<SourceDestBuffer> &sbufs ) :
      cVector_( ni ), isOpen_( false ) // set to true when succeed below
   {
      //???  check if cvector already been written (can't write twice)

      // Empty sbufs is an error
      if ( sbufs.empty() )
      {
         throw E57_EXCEPTION2( ErrorBadAPIArgument, "imageFileName=" + cVector_->imageFileName() +
                                                       " cvPathName=" + cVector_->pathName() );
      }

      // Get CompressedArray's prototype node (all array elements must match this
      // type)
      proto_ = cVector_->getPrototype();

      // Check sbufs well formed (matches proto exactly)
      setBuffers( sbufs ); //??? copy code here?

      // For each individual sbuf, create an appropriate Encoder based on the
      // cVector_ attributes
      for ( unsigned i = 0; i < sbufs_.size(); i++ )
      {
         // Create vector of single sbuf  ??? for now, may have groups later
         std::vector<SourceDestBuffer> vTemp;
         vTemp.push_back( sbufs_.at( i ) );

         ustring codecPath = sbufs_.at( i ).pathName();

         // Calc which stream the given path belongs to.  This depends on position
         // of the node in the proto tree.
         NodeImplSharedPtr readNode = proto_->get( sbufs.at( i ).pathName() );
         uint64_t bytestreamNumber = 0;
         if ( !proto_->findTerminalPosition( readNode, bytestreamNumber ) )
         {
            throw E57_EXCEPTION2( ErrorInternal, "sbufIndex=" + toString( i ) );
         }

         // EncoderFactory picks the appropriate encoder to match type declared in
         // prototype
         bytestreams_.push_back( Encoder::EncoderFactory( static_cast<unsigned>( bytestreamNumber ),
                                                          cVector_, vTemp, codecPath ) );
      }

      // The bytestreams_ vector must be ordered by bytestreamNumber, not by order
      // called specified sbufs, so sort it.
      sort( bytestreams_.begin(), bytestreams_.end(), SortByBytestreamNumber() );
#if ( E57_VALIDATION_LEVEL == VALIDATION_DEEP )
      // Double check that all bytestreams are specified
      for ( unsigned i = 0; i < bytestreams_.size(); i++ )
      {
         if ( bytestreams_.at( i )->bytestreamNumber() != i )
         {
            throw E57_EXCEPTION2( ErrorInternal,
                                  "bytestreamIndex=" + toString( i ) + " bytestreamNumber=" +
                                     toString( bytestreams_.at( i )->bytestreamNumber() ) );
         }
      }
#endif

      ImageFileImplSharedPtr imf( ni->destImageFile_ );

      // Reserve space for CompressedVector binary section header, record location
      // so can save to when writer closes. Request that file be extended with
      // zeros since we will write to it at a later time (when writer closes).
      sectionHeaderLogicalStart_ =
         imf->allocateSpace( sizeof( CompressedVectorSectionHeader ), true );

      sectionLogicalLength_ = 0;
      dataPhysicalOffset_ = 0;
      topIndexPhysicalOffset_ = 0;
      recordCount_ = 0;
      dataPacketsCount_ = 0;
      indexPacketsCount_ = 0;

      // Just before return (and can't throw) increment writer count  ??? safer
      // way to assure don't miss close?
      imf->incrWriterCount();

      // If get here, the writer is open
      isOpen_ = true;
   }

   CompressedVectorWriterImpl::~CompressedVectorWriterImpl()
   {
#ifdef E57_VERBOSE
      std::cout << "~CompressedVectorWriterImpl() called" << std::endl; //???
#endif

      try
      {
         if ( isOpen_ )
         {
            close();
         }
      }
      catch ( ... )
      {
         //??? report?
      }
   }

   void CompressedVectorWriterImpl::close()
   {
#ifdef E57_VERBOSE
      std::cout << "CompressedVectorWriterImpl::close() called" << std::endl; //???
#endif
      ImageFileImplSharedPtr imf( cVector_->destImageFile_ );

      // Before anything that can throw, decrement writer count
      imf->decrWriterCount();

      checkImageFileOpen( __FILE__, __LINE__, static_cast<const char *>( __FUNCTION__ ) );
      // don't call checkWriterOpen();

      if ( !isOpen_ )
      {
         return;
      }

      // Set closed before do anything, so if get fault and start unwinding, don't
      // try to close again.
      isOpen_ = false;

      // If have any data, write packet
      // Write all remaining ioBuffers and internal encoder register cache into
      // file. Know we are done when totalOutputAvailable() returns 0 after a
      // flush().
      flush();
      while ( totalOutputAvailable() > 0 )
      {
         packetWrite();
         flush();
      }

      // Write one index packet (required by standard).
      packetWriteIndex();

      // Compute length of whole section we just wrote (from section start to
      // current start of free space).
      sectionLogicalLength_ = imf->unusedLogicalStart_ - sectionHeaderLogicalStart_;
#ifdef E57_VERBOSE
      std::cout << "  sectionLogicalLength_=" << sectionLogicalLength_ << std::endl; //???
#endif

      // Prepare CompressedVectorSectionHeader
      CompressedVectorSectionHeader header;
      header.sectionLogicalLength = sectionLogicalLength_;
      header.dataPhysicalOffset =
         dataPhysicalOffset_; //??? can be zero, if no data written ???not set yet
      header.indexPhysicalOffset =
         topIndexPhysicalOffset_; //??? can be zero, if no data written ???not set
                                  // yet
#ifdef E57_VERBOSE
      std::cout << "  CompressedVectorSectionHeader:" << std::endl;
      header.dump( 4 ); //???
#endif

#if VALIDATE_BASIC
      // Verify OK before write it.
      header.verify( imf->file_->length( CheckedFile::Physical ) );
#endif

      // Write header at beginning of section, previously allocated
      imf->file_->seek( sectionHeaderLogicalStart_ );
      imf->file_->write( reinterpret_cast<char *>( &header ), sizeof( header ) );

      // Set address and size of associated CompressedVector
      cVector_->setRecordCount( recordCount_ );
      cVector_->setBinarySectionLogicalStart( sectionHeaderLogicalStart_ );

      // Free channels
      bytestreams_.clear();

#ifdef E57_VERBOSE
      std::cout << "  CompressedVectorWriter:" << std::endl;
      dump( 4 );
#endif
   }

   bool CompressedVectorWriterImpl::isOpen() const
   {
      // don't checkImageFileOpen(__FILE__, __LINE__, __FUNCTION__), or
      // checkWriterOpen()
      return isOpen_;
   }

   std::shared_ptr<CompressedVectorNodeImpl> CompressedVectorWriterImpl::compressedVectorNode()
      const
   {
      return cVector_;
   }

   void CompressedVectorWriterImpl::setBuffers( std::vector<SourceDestBuffer> &sbufs )
   {
      // don't checkImageFileOpen

      // If had previous sbufs_, check to see if new ones have changed in
      // incompatible way
      if ( !sbufs_.empty() )
      {
         if ( sbufs_.size() != sbufs.size() )
         {
            throw E57_EXCEPTION2( ErrorBuffersNotCompatible,
                                  "oldSize=" + toString( sbufs_.size() ) +
                                     " newSize=" + toString( sbufs.size() ) );
         }

         for ( size_t i = 0; i < sbufs_.size(); ++i )
         {
            std::shared_ptr<SourceDestBufferImpl> oldbuf = sbufs_[i].impl();
            std::shared_ptr<SourceDestBufferImpl> newBuf = sbufs[i].impl();

            // Throw exception if old and new not compatible
            oldbuf->checkCompatible( newBuf );
         }
      }

      // Check sbufs well formed: no dups, no missing, no extra
      // For writing, all data fields in prototype must be presented for writing
      // at same time.
      proto_->checkBuffers( sbufs, false );

      sbufs_ = sbufs;
   }

   void CompressedVectorWriterImpl::write( std::vector<SourceDestBuffer> &sbufs,
                                           const size_t requestedRecordCount )
   {
      // don't checkImageFileOpen, write(unsigned) will do it
      // don't checkWriterOpen(), write(unsigned) will do it

      setBuffers( sbufs );
      write( requestedRecordCount );
   }

   void CompressedVectorWriterImpl::write( const size_t requestedRecordCount )
   {
#ifdef E57_VERBOSE
      std::cout << "CompressedVectorWriterImpl::write() called" << std::endl; //???
#endif
      checkImageFileOpen( __FILE__, __LINE__, static_cast<const char *>( __FUNCTION__ ) );
      checkWriterOpen( __FILE__, __LINE__, static_cast<const char *>( __FUNCTION__ ) );

      if ( requestedRecordCount == 0 )
      {
         packetWriteZeroRecords();
         return;
      }

      // Check that requestedRecordCount is not larger than the sbufs
      if ( requestedRecordCount > sbufs_.at( 0 ).impl()->capacity() )
      {
         throw E57_EXCEPTION2( ErrorBadAPIArgument,
                               "requested=" + toString( requestedRecordCount ) +
                                  " capacity=" + toString( sbufs_.at( 0 ).impl()->capacity() ) +
                                  " imageFileName=" + cVector_->imageFileName() +
                                  " cvPathName=" + cVector_->pathName() );
      }

      // Rewind all sbufs so start reading from beginning
      for ( auto &sbuf : sbufs_ )
      {
         sbuf.impl()->rewind();
      }

      // Loop until all channels have completed requestedRecordCount transfers
      uint64_t endRecordIndex = recordCount_ + requestedRecordCount;
      while ( true )
      {
         // Calc remaining record counts for all channels
         uint64_t totalRecordCount = 0;
         for ( auto &bytestream : bytestreams_ )
         {
            totalRecordCount += endRecordIndex - bytestream->currentRecordIndex();
         }
#ifdef E57_VERBOSE
         std::cout << "  totalRecordCount=" << totalRecordCount << std::endl; //???
#endif

         // We are done if have no more work, break out of loop
         if ( totalRecordCount == 0 )
         {
            break;
         }

         // Estimate how many records can write before have enough data to fill
         // data packet to efficient length Efficient packet length is >= 75%
         // of maximum packet length. It is OK if get too much data (more than
         // one packet) in an iteration. Reader will be able to handle packets
         // whose streams are not exactly synchronized to the record
         // boundaries. But try to do a good job of keeping the stream
         // synchronization "close enough" (so a reader that can cache only two
         // packets is efficient).

#ifdef E57_VERBOSE
         std::cout << "  currentPacketSize()=" << currentPacketSize() << std::endl; //???
#endif

#ifdef E57_WRITE_CRAZY_PACKET_MODE
         //??? depends on number of streams
         constexpr size_t E57_TARGET_PACKET_SIZE = 500;
#else
         constexpr size_t E57_TARGET_PACKET_SIZE = ( DATA_PACKET_MAX * 3 / 4 );
#endif
         // If have more than target fraction of packet, send it now
         if ( currentPacketSize() >= E57_TARGET_PACKET_SIZE )
         { //???
            packetWrite();
            continue; // restart loop so recalc statistics (packet size may not be
                      // zero after write, if have too much data)
         }

#ifdef E57_VERBOSE
         //??? useful?
         // Get approximation of number of bytes per record of CompressedVector
         // and total of bytes used
         float totalBitsPerRecord = 0; // an estimate of future performance
         for ( auto &bytestream : bytestreams_ )
         {
            totalBitsPerRecord += bytestream->bitsPerRecord();
         }

         const float totalBytesPerRecord = std::max( totalBitsPerRecord / 8, 0.1F ); //??? trust

         std::cout << "  totalBytesPerRecord=" << totalBytesPerRecord << std::endl; //???
#endif

         // Don't allow straggler to get too far behind. ???
         // Don't allow a single channel to get too far ahead ???
         // Process channels that are furthest behind first. ???

         // !!!! For now just process one record per loop until packet is full
         // enough, or completed request
         for ( auto &bytestream : bytestreams_ )
         {
            if ( bytestream->currentRecordIndex() < endRecordIndex )
            {
               // !!! For now, process up to 50 records at a time
               uint64_t recordCount = endRecordIndex - bytestream->currentRecordIndex();
               recordCount =
                  ( recordCount < 50ULL ) ? recordCount : 50ULL; // min(recordCount, 50ULL);
               bytestream->processRecords( static_cast<unsigned>( recordCount ) );
            }
         }
      }

      recordCount_ += requestedRecordCount;

      // When we leave this function, will likely still have data in channel
      // ioBuffers as well as partial words in Encoder registers.
   }

   size_t CompressedVectorWriterImpl::totalOutputAvailable() const
   {
      size_t total = 0;

      for ( const auto &bytestream : bytestreams_ )
      {
         total += bytestream->outputAvailable();
      }

      return total;
   }

   size_t CompressedVectorWriterImpl::currentPacketSize() const
   {
      // Calc current packet size
      return ( sizeof( DataPacketHeader ) + bytestreams_.size() * sizeof( uint16_t ) +
               totalOutputAvailable() );
   }

   uint64_t CompressedVectorWriterImpl::packetWrite()
   {
#ifdef E57_VERBOSE
      std::cout << "CompressedVectorWriterImpl::packetWrite() called" << std::endl; //???
#endif

      // Double check that we have work to do
      const size_t cTotalOutput = totalOutputAvailable();
      if ( cTotalOutput == 0 )
      {
         return ( 0 );
      }

      // const bytestreams_ so it's clear it isn't modified in this function
      const auto &cStreams = bytestreams_;
      const auto cNumByteStreams = cStreams.size();

      // Calc maximum number of bytestream values can put in data packet.
      const size_t cPacketMaxPayloadBytes =
         DATA_PACKET_MAX - sizeof( DataPacketHeader ) - cNumByteStreams * sizeof( uint16_t );

#ifdef E57_VERBOSE
      std::cout << "  totalOutput=" << cTotalOutput << std::endl;
      std::cout << "  cNumByteStreams=" << cNumByteStreams << std::endl;
      std::cout << "  packetMaxPayloadBytes=" << cPacketMaxPayloadBytes << std::endl;
#endif

      // Allocate vector for number of bytes that each bytestream will write to file.
      std::vector<size_t> count( cNumByteStreams );

      // See if we can fit into a single data packet
      if ( cTotalOutput < cPacketMaxPayloadBytes )
      {
         // We can fit everything in one packet
         for ( unsigned i = 0; i < cNumByteStreams; ++i )
         {
            count.at( i ) = cStreams.at( i )->outputAvailable();
         }
      }
      else
      {
         // We have too much data for one packet.  Send proportional amounts from
         // each bytestream. Adjust packetMaxPayloadBytes down by one so have a
         // little slack for floating point weirdness.
         const float cFractionToSend =
            ( cPacketMaxPayloadBytes - 1 ) / static_cast<float>( cTotalOutput );
         for ( unsigned i = 0; i < cNumByteStreams; ++i )
         {
            // Round down here so sum <= packetMaxPayloadBytes
            count.at( i ) = static_cast<unsigned>(
               std::floor( cFractionToSend * cStreams.at( i )->outputAvailable() ) );
         }
      }

#ifdef E57_VERBOSE
      for ( unsigned i = 0; i < cNumByteStreams; ++i )
      {
         std::cout << "  count[" << i << "]=" << count.at( i ) << std::endl;
      }
#endif

#if VALIDATE_BASIC
      // Double check sum of count is <= packetMaxPayloadBytes
      const size_t cTotalByteCount =
         std::accumulate( count.begin(), count.end(), static_cast<size_t>( 0 ) );

      if ( cTotalByteCount > cPacketMaxPayloadBytes )
      {
         throw E57_EXCEPTION2( ErrorInternal,
                               "totalByteCount=" + toString( cTotalByteCount ) +
                                  " packetMaxPayloadBytes=" + toString( cPacketMaxPayloadBytes ) );
      }
#endif

      // Get smart pointer to ImageFileImpl from associated CompressedVector
      ImageFileImplSharedPtr imf( cVector_->destImageFile_ );

      // Use temp buf in object (is 64KBytes long) instead of allocating each time here
      char *packet = reinterpret_cast<char *>( &dataPacket_ );

      // To be safe, clear header part of packet
      dataPacket_.header.reset();

      // Write bytestreamBufferLength[bytestreamCount] after header, in dataPacket_
      auto bsbLength = reinterpret_cast<uint16_t *>( &packet[sizeof( DataPacketHeader )] );
#ifdef E57_VERBOSE
      std::cout << "  packet=" << static_cast<void *>( packet ) << std::endl; //???
      std::cout << "  bsbLength=" << bsbLength << std::endl;                  //???
#endif
      for ( unsigned i = 0; i < cNumByteStreams; ++i )
      {
         bsbLength[i] = static_cast<uint16_t>( count.at( i ) ); // %%% Truncation
#ifdef E57_VERBOSE
         std::cout << "  Writing " << bsbLength[i] << " bytes into bytestream " << i
                   << std::endl; //???
#endif
      }

      // Get pointer to end of data so far
      auto *p = reinterpret_cast<char *>( &bsbLength[cNumByteStreams] );
#ifdef E57_VERBOSE
      std::cout << "  after bsbLength, p=" << static_cast<void *>( p ) << std::endl; //???
#endif

      // Write contents of each bytestream in dataPacket_
      for ( size_t i = 0; i < cNumByteStreams; ++i )
      {
         size_t n = count.at( i );

#if VALIDATE_BASIC
         // Double check we aren't accidentally going to write off end of vector<char>
         if ( &p[n] > &packet[DATA_PACKET_MAX] )
         {
            throw E57_EXCEPTION2( ErrorInternal, "n=" + toString( n ) );
         }
#endif

         // Read from encoder output into packet
         cStreams.at( i )->outputRead( p, n );

         // Move pointer to end of current data
         p += n;
      }

      // Length of packet is difference in beginning pointer and ending pointer
      auto packetLength = static_cast<unsigned>( p - packet ); //??? pointer diff portable?
#ifdef E57_VERBOSE
      std::cout << "  packetLength=" << packetLength << std::endl; //???
#endif

#if VALIDATE_BASIC
      // Double check that packetLength is what we expect
      if ( packetLength !=
           sizeof( DataPacketHeader ) + cNumByteStreams * sizeof( uint16_t ) + cTotalByteCount )
      {
         throw E57_EXCEPTION2( ErrorInternal, "packetLength=" + toString( packetLength ) +
                                                 " bytestreamSize=" +
                                                 toString( cNumByteStreams * sizeof( uint16_t ) ) +
                                                 " totalByteCount=" + toString( cTotalByteCount ) );
      }
#endif

      // packetLength must be multiple of 4, if not, add some zero padding
      while ( packetLength % 4 )
      {
         // Double check we aren't accidentally going to write off end of
         // vector<char>
         if ( p >= &packet[DATA_PACKET_MAX - 1] )
         {
            throw E57_EXCEPTION1( ErrorInternal );
         }
         *p++ = 0;
         packetLength++;
#ifdef E57_VERBOSE
         std::cout << "  padding with zero byte, new packetLength=" << packetLength
                   << std::endl; //???
#endif
      }

      // Prepare header in dataPacket_, now that we are sure of packetLength
      dataPacket_.header.packetLogicalLengthMinus1 =
         static_cast<uint16_t>( packetLength - 1 ); // %%% Truncation
      dataPacket_.header.bytestreamCount =
         static_cast<uint16_t>( cNumByteStreams ); // %%% Truncation

      // Double check that data packet is well formed
      dataPacket_.verify( packetLength );

      // Write whole data packet at beginning of free space in file
      uint64_t packetLogicalOffset = imf->allocateSpace( packetLength, false );
      uint64_t packetPhysicalOffset = imf->file_->logicalToPhysical( packetLogicalOffset );
      imf->file_->seek( packetLogicalOffset ); //??? have seekLogical and seekPhysical instead?
                                               // more explicit
      imf->file_->write( packet, packetLength );

#ifdef E57_VERBOSE
//  std::cout << "data packet:" << std::endl;
//  dataPacket_.dump(4);
#endif

      // If first data packet written for this CompressedVector binary section,
      // save address to put in section header
      //??? what if no data packets?
      //??? what if have exceptions while write, what is state of file?  will
      // close report file
      // good/bad?
      if ( dataPacketsCount_ == 0 )
      {
         dataPhysicalOffset_ = packetPhysicalOffset;
      }
      dataPacketsCount_++;

      // !!! update seekIndex here? if started new chunk?

      // Return physical offset of data packet for potential use in seekIndex
      return ( packetPhysicalOffset ); //??? needed
   }

   // If we don't have any records, write a packet which is only the header + zero padding.
   // Code is a simplified version of packetWrite().
   void CompressedVectorWriterImpl::packetWriteZeroRecords()
   {
      ImageFileImplSharedPtr imf( cVector_->destImageFile_ );

      dataPacket_.header.reset();

      // Use temp buf in object (is 64KBytes long) instead of allocating each time here
      char *packet = reinterpret_cast<char *>( &dataPacket_ );

      auto packetLength = static_cast<unsigned int>( sizeof( DataPacketHeader ) );

      // packetLength must be multiple of 4, add zero padding
      auto data = reinterpret_cast<char *>( &packet[sizeof( DataPacketHeader )] );
      while ( packetLength % 4 )
      {
         *data++ = 0;

         packetLength++;
      }

      // Prepare header in dataPacket_, now that we are sure of packetLength
      dataPacket_.header.packetLogicalLengthMinus1 = static_cast<uint16_t>( packetLength - 1 );

      // Double check that data packet is well formed
      dataPacket_.verify( packetLength );

      // Write packet at beginning of free space in file
      uint64_t packetLogicalOffset = imf->allocateSpace( packetLength, false );
      uint64_t packetPhysicalOffset = imf->file_->logicalToPhysical( packetLogicalOffset );

      imf->file_->seek( packetLogicalOffset );
      imf->file_->write( packet, packetLength );

      // If first data packet written for this CompressedVector binary section,
      // save address to put in section header
      if ( dataPacketsCount_ == 0 )
      {
         dataPhysicalOffset_ = packetPhysicalOffset;
      }

      dataPacketsCount_++;
   }

   // Write one index packet.
   // We don't have an interface to work with index packets, but one is required by the standard, so
   // write one index packet with one entry pointing to the first data packet.
   void e57::CompressedVectorWriterImpl::packetWriteIndex()
   {
      ImageFileImplSharedPtr imf( cVector_->destImageFile_ );

      IndexPacket indexPacket;

      indexPacket.entries[0].chunkPhysicalOffset = dataPhysicalOffset_;

      const auto cPacketLength = sizeof( IndexPacketHeader ) + sizeof( IndexPacket::Entry );

      indexPacket.header.packetLogicalLengthMinus1 = cPacketLength - 1;
      indexPacket.header.entryCount = 1;

      uint64_t packetLogicalOffset = imf->allocateSpace( cPacketLength, false );
      topIndexPhysicalOffset_ = imf->file_->logicalToPhysical( packetLogicalOffset );

      imf->file_->seek( packetLogicalOffset );
      imf->file_->write( reinterpret_cast<const char *>( &indexPacket ), cPacketLength );

      indexPacketsCount_++;
   }

   void CompressedVectorWriterImpl::flush()
   {
      for ( auto &bytestream : bytestreams_ )
      {
         bytestream->registerFlushToOutput();
      }
   }

   void CompressedVectorWriterImpl::checkImageFileOpen( const char *srcFileName, int srcLineNumber,
                                                        const char *srcFunctionName ) const
   {
      // unimplemented...
      E57_UNUSED( srcFileName );
      E57_UNUSED( srcLineNumber );
      E57_UNUSED( srcFunctionName );
   }

   void CompressedVectorWriterImpl::checkWriterOpen( const char *srcFileName, int srcLineNumber,
                                                     const char *srcFunctionName ) const
   {
      if ( !isOpen_ )
      {
         throw E57Exception( ErrorWriterNotOpen,
                             "imageFileName=" + cVector_->imageFileName() +
                                " cvPathName=" + cVector_->pathName(),
                             srcFileName, srcLineNumber, srcFunctionName );
      }
   }

#ifdef E57_ENABLE_DIAGNOSTIC_OUTPUT
   void CompressedVectorWriterImpl::dump( int indent, std::ostream &os )
   {
      os << space( indent ) << "isOpen:" << isOpen_ << std::endl;

      for ( unsigned i = 0; i < sbufs_.size(); i++ )
      {
         os << space( indent ) << "sbufs[" << i << "]:" << std::endl;
         sbufs_.at( i ).dump( indent + 4, os );
      }

      os << space( indent ) << "cVector:" << std::endl;
      cVector_->dump( indent + 4, os );

      os << space( indent ) << "proto:" << std::endl;
      proto_->dump( indent + 4, os );

      for ( unsigned i = 0; i < bytestreams_.size(); i++ )
      {
         os << space( indent ) << "bytestreams[" << i << "]:" << std::endl;
         bytestreams_.at( i )->dump( indent + 4, os );
      }

      // Don't call dump() for DataPacket, since it may contain junk when
      // debugging.  Just print a few byte values.
      os << space( indent ) << "dataPacket:" << std::endl;
      auto p = reinterpret_cast<uint8_t *>( &dataPacket_ );

      for ( unsigned i = 0; i < 40; ++i )
      {
         os << space( indent + 4 ) << "dataPacket[" << i << "]: " << static_cast<unsigned>( p[i] )
            << std::endl;
      }
      os << space( indent + 4 ) << "more unprinted..." << std::endl;

      os << space( indent ) << "sectionHeaderLogicalStart: " << sectionHeaderLogicalStart_
         << std::endl;
      os << space( indent ) << "sectionLogicalLength:      " << sectionLogicalLength_ << std::endl;
      os << space( indent ) << "dataPhysicalOffset:        " << dataPhysicalOffset_ << std::endl;
      os << space( indent ) << "topIndexPhysicalOffset:    " << topIndexPhysicalOffset_
         << std::endl;
      os << space( indent ) << "recordCount:               " << recordCount_ << std::endl;
      os << space( indent ) << "dataPacketsCount:          " << dataPacketsCount_ << std::endl;
      os << space( indent ) << "indexPacketsCount:         " << indexPacketsCount_ << std::endl;
   }
#endif
}
