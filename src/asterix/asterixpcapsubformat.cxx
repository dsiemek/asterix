/*
 *  Copyright (c) 2013 Croatia Control Ltd. (www.crocontrol.hr)
 *
 *  This file is part of Asterix.
 *
 *  Asterix is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Asterix is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Asterix.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * AUTHORS: Damir Salantic, Croatia Control Ltd.
 *
 */

#include <stdio.h>
#include <string.h>

#include "../main/asterix.h"
#include "asterixformat.hxx"
#include "asterixformatdescriptor.hxx"
#include "asterixpcapsubformat.hxx"

#include "AsterixDefinition.h"
#include "InputParser.h"

bool CAsterixPcapSubformat::ReadPacket(CBaseFormatDescriptor &formatDescriptor, CBaseDevice &device, bool &discard, bool oradis)
{
  CAsterixFormatDescriptor& Descriptor((CAsterixFormatDescriptor&)formatDescriptor);
  unsigned short m_nDataLength = 0;

  if (!Descriptor.m_bFileInitialized)
  {
    pcap_hdr_t m_ePcapFileHeader;

    // Read file header
    if (!device.Read((void*)&m_ePcapFileHeader, sizeof(m_ePcapFileHeader)))
    {
        LOGERROR(1, "Couldn't read PCAP file header.\n");
        return false;
    }

    // find input file format
    if (m_ePcapFileHeader.magic_number == 0xA1B2C3D4)
    {
      Descriptor.m_bInvertByteOrder = false;
    }
    else if (m_ePcapFileHeader.magic_number == 0xD4C3B2A1)
    {
      Descriptor.m_bInvertByteOrder = true;

      m_ePcapFileHeader.network = convert_long(m_ePcapFileHeader.network);
    }
    else
    {
      LOGERROR(1, "Unknown input file format");
      return false;
    }

    if (m_ePcapFileHeader.network == 1)
    {
      Descriptor.m_ePcapNetworkType = CAsterixFormatDescriptor::NET_ETHERNET;
    }
    else if (m_ePcapFileHeader.network == 113)
    {
      Descriptor.m_ePcapNetworkType = CAsterixFormatDescriptor::NET_LINUX;
    }
    else
    {
      LOGERROR(1, "Unknown network type in PCAP format");
      return false;
    }

    Descriptor.m_bFileInitialized = true;
  }

  ////////////// Read PCAP packet header
  //////////////////////////////////////////////////////////////////////////////////////
  pcaprec_hdr_t m_ePcapRecHeader;

  if (!device.Read((void*)&m_ePcapRecHeader, sizeof(m_ePcapRecHeader)))
  {
      LOGERROR(1, "Couldn't read PCAP header.\n");
      return false;
  }

  // read PCAP packet
  unsigned long nPacketBufferSize = m_ePcapRecHeader.incl_len;
  if (Descriptor.m_bInvertByteOrder)
  {
    nPacketBufferSize = convert_long(nPacketBufferSize);
  }
  unsigned char* pPacketBuffer = new unsigned char[nPacketBufferSize]; // input data buffer
  if (!device.Read((void*)pPacketBuffer, nPacketBufferSize))
  {
      LOGERROR(1, "Couldn't read PCAP packet.\n");
      delete pPacketBuffer;
      return false;
  }

  ////////////// Parse PCAP packet header
  //////////////////////////////////////////////////////////////////////////////////////
  unsigned char* pPacketPtr = pPacketBuffer;
  if (Descriptor.m_ePcapNetworkType == CAsterixFormatDescriptor::NET_ETHERNET)
  { // Ethernet header
    pPacketPtr += 6; // Destination MAC
    pPacketPtr += 6; // Source MAC
  }
  else if (Descriptor.m_ePcapNetworkType == CAsterixFormatDescriptor::NET_LINUX)
  { // Linux cooked packet
    pPacketPtr += 2; // Packet type
    pPacketPtr += 2; // link-layer Address type
    pPacketPtr += 2; // Address length
    pPacketPtr += 8; // Source
  }

  unsigned short protoType = *((unsigned short*)pPacketPtr);
  pPacketPtr += 2;

  if (Descriptor.m_bInvertByteOrder)
  {
    protoType = convert_short(protoType);
  }

  // check if byte order should be reverted
  bool bIPInvertByteOrder = Descriptor.m_bInvertByteOrder;
  if (protoType == 0x8)
  {
    bIPInvertByteOrder = !Descriptor.m_bInvertByteOrder;
  }
  else if (protoType != 0x800)
  {
    LOGERROR(1, "Unknown protocol type (%x)\n", protoType);
    delete pPacketBuffer;
    return false;
  }

  ////////////// Parse IP header
  //////////////////////////////////////////////////////////////////////////////////////
  unsigned char IPheaderLength = *pPacketPtr;
  pPacketPtr++;

  IPheaderLength &= 0x0F;

  pPacketPtr += 1; //TOS

  unsigned short IPtotalLength = *((unsigned short*)pPacketPtr); // Total length
  pPacketPtr += 2;

  if (bIPInvertByteOrder)
  {
    IPtotalLength = convert_short(IPtotalLength);
  }

  pPacketPtr += 5; // ID + Flags + Offset + TTL

  unsigned char protocol = *pPacketPtr;
  pPacketPtr++;

  if (protocol != 17)
  { // TODO only UDP supported for now
    pPacketPtr += IPtotalLength-10;
    LOGERROR(1, "Unsupported protocol type (%x)", protocol);
    delete pPacketBuffer;
    return false;
  }

  pPacketPtr += 2; // checksum
  pPacketPtr += 4; // source IP
  pPacketPtr += 4; // destination IP

  pPacketPtr += IPheaderLength*4-20; // options

  IPtotalLength -= IPheaderLength*4;

  if (protocol == 17)
  { // UDP header
    pPacketPtr += 2; // source port
    pPacketPtr += 2; // destination port

    m_nDataLength = *((unsigned short*)pPacketPtr); // length
    pPacketPtr += 2;

    if (bIPInvertByteOrder)
    {
      m_nDataLength = convert_short(m_nDataLength);
    }

    if (IPtotalLength != m_nDataLength)
    {
      LOGERROR(1, "Wrong UDP data length");
      delete pPacketBuffer;
      return false;
    }
    pPacketPtr += 2; // checksum

    m_nDataLength -= 8;
  }

  // parse packet
  if (Descriptor.m_pAsterixData)
  {
    delete Descriptor.m_pAsterixData;
    Descriptor.m_pAsterixData = NULL;
  }

  if (oradis)
  {
    while (m_nDataLength > 0)
    {
      // parse ORADIS header (6 bytes)
      // Byte count (1) (MSB)
      // Byte count (2) (LSB)
      // Time (1) MSB
      // Time (2)
      // Time (3)
      // Time (4) LSB
      // ASTERIX (byte counts)
      unsigned short byteCount = *pPacketPtr; // length
      pPacketPtr ++;
      byteCount <<= 8;
      byteCount |= *pPacketPtr;
      pPacketPtr ++;

      // skip time
      pPacketPtr +=4;

      if (byteCount > m_nDataLength)
        break;

      // Parse ASTERIX data
      AsterixData* m_ptmpAsterixData = Descriptor.m_InputParser.parsePacket(pPacketPtr, byteCount-6);

      if (Descriptor.m_pAsterixData == NULL)
      {
        Descriptor.m_pAsterixData = m_ptmpAsterixData;
      }
      else
      {
        // merge Asterix packet
        Descriptor.m_pAsterixData->m_lDataBlocks.merge(m_ptmpAsterixData->m_lDataBlocks);
      }

      pPacketPtr += (byteCount-6);
      m_nDataLength -= byteCount;
    }
  }
  else
  {
    Descriptor.m_pAsterixData = Descriptor.m_InputParser.parsePacket(pPacketPtr, m_nDataLength);
  }

  delete pPacketBuffer;
  return true; //TODO
}

bool CAsterixPcapSubformat::WritePacket(CBaseFormatDescriptor &formatDescriptor, CBaseDevice &device, bool &discard, bool oradis)
{
  return false; //TODO
}

bool CAsterixPcapSubformat::ProcessPacket(CBaseFormatDescriptor &formatDescriptor, CBaseDevice &device, bool &discard, bool oradis)
{
  return true;
}

bool CAsterixPcapSubformat::Heartbeat(CBaseFormatDescriptor &formatDescriptor, CBaseDevice &device, bool oradis)
{
  return true;
}
