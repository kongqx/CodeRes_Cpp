#include "zigbee_common.h"


const ZclCmd ZclGenOnOff::cmds[ZclGenOnOff::N_CMD] = {
										{ (uint8)ZclGenOnOff::OFF, "Off", ZclGenOnOff::off },
										{ (uint8)ZclGenOnOff::ON, "On", ZclGenOnOff::on },
										{ (uint8)ZclGenOnOff::TOGGLE, "Toggle", ZclGenOnOff::toggle } 
									};



int zigbee_fd;
ZigbeeDevice thisDev;
// device list
ZigbeeDeviceList zigbeeDevList;

/* static global vars */
// thread id of working routine
static pthread_t				routine_tid;
/* reading buffer for working routine */
static char readbuf[ZIGBEE_BUFLEN];
static char *readPtr = readbuf;
// control the routine thread, initialized in zigbee_init()
static volatile uint8			running;
// at command callback table, for zigbee_at_cmd
static AtCmdCallbackTable atCmdCallbackTable;
// at command callback list, for receiving any incomming notification
static AtCmdCallbackList atCmdCallbackList;
// at command callback global pointer, for only one at command can be run at one time
static volatile AtCmdCallbackPtr atCmdSyncCallbackPtr = 0;
// General callback pernament list, for dealing with notifications generated by the device
static GeneralCallbackList genCbList;
// General callback temporary list, for dealing responses of the zcl or zdp request
// the processor of callback will call its find_remove() to get a match cb.
// and if this cb isn't sync, the processor will delete it after job is done.
static GeneralCallbackSafeList genCbTmpList;


/* functions for debugging */
#ifdef _DEBUG 
void printAddr( const Address_t *addr )
{
	if( addr->type == EXTENDED )
		pr_mem( addr->addrData.ieeeAddr, 8 );
	else
		DBG( "%04x", addr->addrData.shortAddr );
}

//static
//void printZclFrame( const ZclFrame_t &frame )
//{
//	printf( "ZCL Frame:\n" );
//	printf( "frameCtrl:\t\t\t%02x\n", frame.frameCtrl );
//	if( frame.frameCtrl & ZCL_FRAME_MANU_SPEC )
//		printf( "manufacturerCode:\t\t\t%04x\n", frame.manufacturerCode );
//	printf( "seqNO:\t\t\t%02x\n", frame.seqNO );
//	printf( "cmd:\t\t\t%02x\n", frame.cmd );
//	printf( "payloadLen:\t\t\t%u\n", frame.payloadLen );
//	if( frame.payload ) {
//		printf( "payload: " );
//		pr_mem( frame.payload, frame.payloadLen );
//	} else
//		printf( "payload: NULL\n" );
//}

void printDevList()
{
	for( ZigbeeDeviceList::iterator it = zigbeeDevList.begin(); 
			it != zigbeeDevList.end(); ++it ) {
		(*it)->print();
		putchar('\n');
	}
}
#endif

//!! free cb data, normally it's the user who waiting for a callback's return to free its cbData
// mem, but once wait fail like timeout, the cb obj itself must free those mem in its destructor
// pass these function ptr to the CB obj by its setCbData()
//!! typname T must be a pointer.
template < typename T >
static void deleteCbData( void *vptr, bool isArray )
{
	T ptr = (T)vptr;

//	DBG( "deleteCbData called, %s", isArray ? "array" : "not array" );
	
	if( isArray )
		delete [] ptr;
	else
		delete ptr;
}

static inline
void freeRawCbData( void *vptr, bool )
{
	free(vptr);
}


inline
bool operator== ( const Address_t &lhs, const Address_t &rhs )
{
	if( lhs.type != rhs.type )
		return false;
	if( NOTPRESENT == lhs.type )
		return true;
	if( EXTENDED == lhs.type )
		return memcmp( lhs.addrData.ieeeAddr, rhs.addrData.ieeeAddr, 8 ) == 0;
	return lhs.addrData.shortAddr == rhs.addrData.shortAddr;
}

inline
bool operator!= ( const Address_t &lhs, const Address_t &rhs )
{
	return !(lhs == rhs);
}


ssize_t tread(int fd, void *buf, size_t nbytes, unsigned int timout)
{
	int				nfds;
	fd_set			readfds;
	struct timeval	tv;

	tv.tv_sec = timout;
	tv.tv_usec = 0;
	FD_ZERO(&readfds);
	FD_SET(fd, &readfds);
	nfds = select(fd+1, &readfds, NULL, NULL, &tv);
	if (nfds <= 0) {
		if (nfds == 0)
			errno = ETIME;
		return(-1);
	}
	return(read(fd, buf, nbytes));
}

ssize_t twrite(int fd, const void *buf, size_t nbytes, unsigned int timout)
{
	int				nfds;
	fd_set			writefds;
	struct timeval	tv;

	tv.tv_sec = timout;
	tv.tv_usec = 0;
	FD_ZERO(&writefds);
	FD_SET(fd, &writefds);
	nfds = select(fd+1, &writefds, NULL, NULL, &tv);
	if (nfds <= 0) {
		if (nfds == 0)
			errno = ETIME;
		return(-1);
	}
	return(write(fd, buf, nbytes));
}

ssize_t	readn(int fd, void *vptr, size_t n)
{
	size_t	nleft;
	ssize_t	nread;
	char	*ptr;

	ptr = (char*)vptr;
	nleft = n;
	while (nleft > 0) {
		if ( (nread = read(fd, ptr, nleft)) < 0) {
			if (errno == EINTR)
				nread = 0;		/* and call read() again */
			else
				return(-1);
		} else if (nread == 0)
			break;				/* EOF */

		nleft -= nread;
		ptr   += nread;
	}

	pr_mem2( vptr, n - nleft );
	return(n - nleft);		/* return >= 0 */
}

ssize_t treadn(int fd, void *buf, size_t nbytes, unsigned int timout)
{
	size_t	nleft;
	ssize_t	nread;

	nleft = nbytes;
	while (nleft > 0) {
		if ((nread = tread(fd, buf, nleft, timout)) < 0) {
			if (nleft == nbytes)
				return(-1); /* error, return -1 */
			else
				break;      /* error, return amount read so far */
		} else if (nread == 0) {
			break;          /* EOF */
		}
		nleft -= nread;
//		buf += nread;
        buf = (char*)buf + nread;
	}
	return(nbytes - nleft);      /* return >= 0 */
}

ssize_t writen(int fd, const void *ptr, size_t n)
{
	size_t		nleft;
	ssize_t		nwritten;

	nleft = n;
	while (nleft > 0) {
		if ((nwritten = write(fd, ptr, nleft)) < 0) {
			if (nleft == n)
				return(-1); /* error, return -1 */
			else
				break;      /* error, return amount written so far */
		} else if (nwritten == 0) {
			break;
		}
		nleft -= nwritten;
//		ptr   += nwritten;
        ptr = (char*)ptr + nwritten;
	}
	return(n - nleft);      /* return >= 0 */
}


// Non-Canonical not text based
//int open_zigbee_device( const char *dev_name, int baudrate )
//{
//	int fd;

//	fd = OpenSerial(fd, dev_name);
//	if( fd < 0 )
//		return fd;

//	if( InitSerial(fd,115200,0,8,1,'N') < 0 ) {
//		perror( "InitSerial error!" );
//		return -1;
//	}

//	zigbee_fd = fd;

//	return fd;
//}

static
char *read_a_word( int fd, char *buf, char retval = 0 )
{
	char *ptr = buf;
	char data;

	while( readn( fd, &data, 1 ) == 1 ) {
		if( retval && data == retval )
			return (char*)retval;
		if( isspace(data) ) {
			if( ptr != buf ) {
				*ptr = 0;
				return buf;
			} // if ptr != buf
		} else
			*ptr++ = data;
	} // while

	return (char*)NULL;		// read error
}

//static
//char *read_a_line( int fd, char *buf )
//{
//	char *ptr = buf;
//	char data;

//	// skip leading spaces
//	do {
//		if( readn(fd, &data, 1) != 1 )
//			goto FAIL; 
//	} while( isspace(data) );
//	*ptr++ = data;

//	while( readn( fd, &data, 1 ) == 1 ) {
//		if( '\r' == data || '\n' == data ) {
//			*ptr = 0;
//			return buf;
//		} else
//			*ptr++ = data;
//	} // while

//FAIL:
//	return (char*)NULL;
//}

static
uint8* osal_buffer_uint32( uint8 *buf, uint32 val )
{
  *buf++ = BREAK_UINT32( val, 0 );
  *buf++ = BREAK_UINT32( val, 1 );
  *buf++ = BREAK_UINT32( val, 2 );
  *buf++ = BREAK_UINT32( val, 3 );

  return buf;
}


static
uint8 *parseZclData( uint8 dataType, void *attrData, uint8 *buf )
{
  uint8 *pStr;
  uint16 len;

  switch ( dataType )
  {
    case ZCL_DATATYPE_DATA8:
    case ZCL_DATATYPE_BOOLEAN:
    case ZCL_DATATYPE_BITMAP8:
    case ZCL_DATATYPE_INT8:
    case ZCL_DATATYPE_UINT8:
    case ZCL_DATATYPE_ENUM8:
      *buf++ = *((uint8 *)attrData);
       break;

    case ZCL_DATATYPE_DATA16:
    case ZCL_DATATYPE_BITMAP16:
    case ZCL_DATATYPE_UINT16:
    case ZCL_DATATYPE_INT16:
    case ZCL_DATATYPE_ENUM16:
    case ZCL_DATATYPE_SEMI_PREC:
    case ZCL_DATATYPE_CLUSTER_ID:
    case ZCL_DATATYPE_ATTR_ID:
      *buf++ = LO_UINT16( *((uint16*)attrData) );
      *buf++ = HI_UINT16( *((uint16*)attrData) );
      break;

    case ZCL_DATATYPE_DATA24:
    case ZCL_DATATYPE_BITMAP24:
    case ZCL_DATATYPE_UINT24:
    case ZCL_DATATYPE_INT24:
      *buf++ = BREAK_UINT32( *((uint32*)attrData), 0 );
      *buf++ = BREAK_UINT32( *((uint32*)attrData), 1 );
      *buf++ = BREAK_UINT32( *((uint32*)attrData), 2 );
      break;

    case ZCL_DATATYPE_DATA32:
    case ZCL_DATATYPE_BITMAP32:
    case ZCL_DATATYPE_UINT32:
    case ZCL_DATATYPE_INT32:
    case ZCL_DATATYPE_SINGLE_PREC:
    case ZCL_DATATYPE_TOD:
    case ZCL_DATATYPE_DATE:
    case ZCL_DATATYPE_UTC:
    case ZCL_DATATYPE_BAC_OID:
      buf = osal_buffer_uint32( buf, *((uint32*)attrData) );
      break;

    case ZCL_DATATYPE_UINT40:
      pStr = (uint8*)attrData;
      buf = (uint8*)memcpy( buf, pStr, 5 );
      break;

    case ZCL_DATATYPE_UINT48:
      pStr = (uint8*)attrData;
      buf = (uint8*)memcpy( buf, pStr, 6 );
      break;

    case ZCL_DATATYPE_IEEE_ADDR:
      pStr = (uint8*)attrData;
      buf = (uint8*)memcpy( buf, pStr, 8 );
      break;

    case ZCL_DATATYPE_CHAR_STR:
    case ZCL_DATATYPE_OCTET_STR:
      pStr = (uint8*)attrData;
      len = *pStr;
      buf = (uint8*)memcpy( buf, pStr, len+1 ); // Including length field
      break;

    case ZCL_DATATYPE_LONG_CHAR_STR:
    case ZCL_DATATYPE_LONG_OCTET_STR:
      pStr = (uint8*)attrData;
      len = BUILD_UINT16( pStr[0], pStr[1] );
      buf = (uint8*)memcpy( buf, pStr, len+2 ); // Including length field
      break;

    case ZCL_DATATYPE_128_BIT_SEC_KEY:
      pStr = (uint8*)attrData;
      buf = (uint8*)memcpy( buf, pStr, SEC_KEY_LEN );
      break;

    case ZCL_DATATYPE_NO_DATA:
    case ZCL_DATATYPE_UNKNOWN:
      // Fall through

    default:
      break;
  }

  return ( buf );
}

ZclBase *createZcl( uint16 clusterID )
{
	switch( clusterID ) {
	case ZCL_CLUSTER_ID_GEN_ON_OFF:
		return new ZclGenOnOff;
	case ZCL_CLUSTER_ID_UBEC_POWERMETER:
		return new ZclUbecPowerMeter;
	}

	return NULL;
}

uint8 make_address( Address_t *addr, AddrType_t addrType, void *data )
{
    uint8 len;
    
	memset( addr, 0, sizeof(*addr) );
	addr->type = addrType;
	if( NOTPRESENT == addrType )
		return 0;
    
	if( EXTENDED == addrType ) {
		memcpy( addr->addrData.ieeeAddr, data, 8 );
        len = 8;
	} else {
		addr->addrData.shortAddr = to_u16(data);
        len = 2;
	}
    
	return len;
}

static
uint8 get_checksum( uint8 *start, uint8 *end )
{
	uint8 checksum = 0;
	uint8 *p;
    
	for( p = start; p != end; ++p )
		checksum += *p;
	checksum &= 0xFF;
	checksum = 0xFF - checksum;
    
	return checksum;
}

static
int parseAtIeee( void *arg, Callback *cb )
{
	DBG( "parseAtIeee TODO" );

	if( cb )
		cb->finish( Callback::SUCCESS, 0 );	
	return 0;
}

static
int parseAtDump( void *arg, Callback *cb )
{
	char				*word;
	uint16				panID;
	ZigbeeDevicePtr		pDev = 0;

	DBG( "parseAtDump Called." );

	do {
		if( (word = read_a_word( zigbee_fd, readbuf )) == NULL )
			goto FAIL;

		if( strcmp(word, "PanID") == 0 ) {
			if( (word = read_a_word( zigbee_fd, readbuf )) == NULL )
				goto FAIL;
			panID = (uint16)strtol( word, NULL, 16 );
			if( panID != thisDev.panId() )
				continue;
			else {
				if( pDev )
					zigbeeDevList.add( pDev );
				pDev = new ZigbeeDevice;
			}
		} // panid

		if( strcmp(word, "MAC") == 0 ) {
			if( (word = read_a_word( zigbee_fd, readbuf )) == NULL )
				goto FAIL;
			if( strcmp(word, "Addr") != 0 )
				continue;
			// mac addr data
			if( (word = read_a_word( zigbee_fd, readbuf )) == NULL )
				goto FAIL;
			pDev->setMacAddr( word );			
		} // mac addr

		if( strcmp(word, "NetworkAddress") == 0 ) {
			if( (word = read_a_word( zigbee_fd, readbuf )) == NULL )
				goto FAIL;
			pDev->setAddr( (uint16)strtol( word, NULL, 16 ) );
		}

		if( strcmp(word, "DeviceType") == 0 ) {
			if( (word = read_a_word( zigbee_fd, readbuf )) == NULL )
				goto FAIL;
			pDev->setRole( strtol( word, NULL, 16 ) );
		}

		if( strcmp(word, "LogicalChannel") == 0 ) {
			if( (word = read_a_word( zigbee_fd, readbuf )) == NULL )
				goto FAIL;
			pDev->setChannel( (uint8)strtol( word, NULL, 16 ) );
		}		

		if( strcmp(word, "Security") == 0 ) {
			if( (word = read_a_word( zigbee_fd, readbuf )) == NULL )
				goto FAIL;
			pDev->setSecurity( (uint8)strtol( word, NULL, 16 ) );
		}		
	} while( strcmp(word, "OK") != 0 );

	if( pDev )
		zigbeeDevList.add( pDev );	

	if( cb )
		cb->finish( Callback::SUCCESS, 0 );
	return 0;
FAIL:
	DBG( "parseAtDump fail." );
	if( cb )
		cb->finish( Callback::FAIL, -1 );	
	delete pDev;
	return -1;
}

static
int parseNewJoin( void *arg, Callback *cb )
{
	int					i;
	char				*word;

	DBG( "parseNewJoin Called." );

	// 2 fields, long short addr
	for( i = 1; i <= 2; ++i ) {
		word = read_a_word( zigbee_fd, readbuf );
		if( !word )
			goto FAIL;	
		switch(i) {
		case 1:
			DBG( "parseNewJoin, Mac Addr: %s", word );
			break;
		case 2:
			DBG( "parseNewJoin, Short Addr: %s", word );
			break;
		} // switch
	} // for

	return 0;
FAIL:
	return -1;
}

static
int parseAtInfo( void *arg, Callback *cb )
{
	char					*word;
	int						i, j, ret;
	uint8 					data, *pDst;
	std::auto_ptr<ZigbeeDevice> 	devInfo;
	char 					tmp[3], *pSrc;
	uint8					macAddr[8];

	DBG( "parseAtInfo" );

	devInfo.reset( new ZigbeeDevice );
	if( !devInfo.get() )
		goto FAIL;

	// totally 7 fields
	for( i = 1; i <= 7; ++i ) {
		word = read_a_word( zigbee_fd, readbuf );
		if( !word )
			goto FAIL;
		switch(i) {
		case 1:
			devInfo->setChannel( (uint8)strtol( word, NULL, 16 ) );
			break;
		case 2:
			devInfo->setPanId( (uint16)strtol( word, NULL, 16 ) );
			break;
		case 3:	
			devInfo->setRole( (ZigbeeDevType_t)strtol( word, NULL, 16 ) );
			break;
		case 4:	
			devInfo->setAddr( (uint16)strtol( word, NULL, 16 ) );
			break;
		case 5:		// mac addr
			if( strlen(word) != 16 )
				goto FAIL;
//			pDst = macAddr;		
//			tmp[2] = 0;
//			for( j = 0, pSrc = word; j < 8; ++j, pSrc += 2 ) {
//				strncpy( tmp, pSrc, 2 );
//				data = (uint8)strtol(tmp, NULL, 16);
//				*pDst++ = data;
//			} // for
			devInfo->setMacAddr( word );
			break;
		case 6:
			devInfo->setP2pAddr( (uint16)strtol( word, NULL, 16 ) );
			break;
		case 7:
			devInfo->setSecurity( (uint8)strtol( word, NULL, 16 ) );
			break;
		} // switch
	} // for

//	devInfo->printThisDevice();

	if( cb ) {
		cb->setCbData( devInfo.release(), deleteCbData<ZigbeeDevice*> );
		cb->finish( Callback::SUCCESS, 0 );
	}
	return 0;
FAIL:
	DBG( "parseAtInfo fail." );
	if( cb )
		cb->finish( Callback::FAIL, -1 );
	return -1;
}


static
int parseAtCmdRsp( const char *cmdStr )
{
	AtCmdCallbackPtr ptr = 0;
	int ret;
	
//	DBG( "parseAtCmdRsp %s", cmdStr );

//#ifdef _DEBUG
//	if( atCmdSyncCallbackPtr )
//		DBG( "atCmdSyncCallbackPtr NAME = %s", atCmdSyncCallbackPtr->name() );
//	else
//		DBG( "atCmdSyncCallbackPtr is NULL" );
//#endif

	if( atCmdSyncCallbackPtr && strcmp(atCmdSyncCallbackPtr->name(), cmdStr) == 0 )
		ptr = atCmdSyncCallbackPtr;
	else {
		// this list now temporarily unavailable
		for( AtCmdCallbackList::iterator it = atCmdCallbackList.begin(); 
				it != atCmdCallbackList.end(); ++it ) {
			if( strcmp(cmdStr, (*it)->name()) == 0 ) {
				ptr = *it;
				break;
			} // if
		} // for
	}

	if( !ptr ) {
//		DBG( "No callback processor found." );
		return -1;
	}

	CallbackProcessorFunc funcPtr = ptr->routinePtr();
	if( funcPtr )
		ret = funcPtr( NULL, ptr );

	if( ret ) {
		DBG( "callback processor fail, %d", ret );
		return ret;
	}
	
	return 0;
}

static
int buildZclFrame( ZclFrame &frame, const uint8 *start, const uint8 *end )
{
	const uint8 		*ptr = start;
	uint16				manuCode;

	frame.setCtrl( *ptr++ );
	
	if( frame.isManuSpec() ) {
		manuCode = to_u16( ptr );
		ptr += 2;
#ifdef HOST_ZCL_ENDIAN_DIFF
		BYTESWAP( &manuCode, uint16 );
#endif
		frame.setManuCode( manuCode );
	} // if

	frame.setSeqNO( *ptr++ );
	frame.setCmd( *ptr++ );
	frame.setPayload( ptr, end );	

	return (end - start);
}

static
int parseZclCB( void *arg, Callback *cb )
{
	ZclFrame *pFrame = (ZclFrame*)arg;

	DBG( "parseZclCB: " );
//#ifdef _DEBUG
//	printZclFrame( *pFrame );
//#endif

	if( cb ) {
		if( !(cb->getWaitFlag()) ) {
			DBG( "Delete non waiting zcl callback." );
			delete cb;
		} else {		
			cb->setCbData( pFrame, deleteCbData<ZclFrame*> );
			cb->finish( Callback::SUCCESS, 0 );
		} // if cb waitflag
	} // if cb
		
	return 0;
FAIL:
	if( cb ) {
		if( !(cb->getWaitFlag()) )
			delete cb;
		else
			cb->finish( Callback::FAIL, -1 );
	} // if cb
	return -1;
}

static
int parseGenRsp()
{
	static uint8	buf[ZIGBEE_BUFLEN];
	int				ret = 0;
	uint16			length;
	Address_t		addr;
	AddrType_t		addrType;
    uint8           addrLen;
	uint8			cmd;
	uint8 			*ptr = buf;
	uint8			*pRead;
	uint8			srcep, dstep;
	uint16			profileID, clusterID;
	std::auto_ptr<ZclFrame> 	pFrame;
	GeneralCallbackPtr 		pcb;

	DBG( "parseGenRsp" );

	// read length, buf start with "length"
	if( readn( zigbee_fd, ptr, 2 ) != 2 ) {
		DBG( "parseRawDRsp read error, %s", strerror(errno) );
		return -1;
	}
	ptr += 2;

	length = to_u16(buf);
#ifdef HOST_ZDEV_ENDIAN_DIFF
	BYTESWAP( &length, uint16 );
#endif

	//!! I'm not sure the max len of ubec serial frame
	if( length > ZIGBEE_BUFLEN ) {
		DBG( "Invalid length: %u", length );
		return 1;
	}

	// read the rest length + 2 bytes of the frame
	if( readn( zigbee_fd, ptr, length+2 ) != length+2 ) {
		DBG( "parseRawDRsp read frame error, %s", strerror(errno) );
		return -1;
	}
	ptr += length + 1; // point to the End byte
	if( *ptr != 0x03 ) {
		DBG( "Invalid EOF, %02x", *ptr );
		return 2;
	}

	// checksum
	--ptr;
	if( *ptr != get_checksum(buf, ptr) ) {
		DBG( "check sum fail." );
		return 3;
	}

	cmd = buf[2];		// buf start with "length"

	// address
	addrType = (AddrType_t)( cmd >> 6 & 0x03 );
	addrLen = make_address( &addr, addrType, buf + 3 );
#ifdef HOST_ZDEV_ENDIAN_DIFF
	if( addr.type != EXTENDED )
		BYTESWAP( &(addr.addrData.shortAddr), uint16 );
#endif
	pRead = buf + 3 + addrLen;		// start of the data (req)
	// cmd type
	if( (cmd & API_RAW) == API_RAW ) {
		// read data req
		srcep = *pRead++;
		dstep = *pRead++;
		profileID = to_u16(pRead);
#ifdef HOST_ZDEV_ENDIAN_DIFF
		BYTESWAP( &profileID, uint16 );
#endif
		pRead += 2;
		clusterID = to_u16(pRead);
#ifdef HOST_ZDEV_ENDIAN_DIFF
		BYTESWAP( &clusterID, uint16 );
#endif
		pRead += 2; 	// pRead now point to the data

		if( profileID ) {
			// zcl
			pFrame.reset( new ZclFrame );
			buildZclFrame( *pFrame, pRead, ptr );
#ifdef _DEBUG
			DBG( "Received zcl frame: " );
			printf( "Addr: " );
			printAddr( &addr );
			DBG( "profileID: %04x", profileID );
			DBG( "clusterID: %04x", clusterID );
			DBG( "srcep: %02x", srcep );
			DBG( "dstep: %02x", dstep );
			pFrame->print();
#endif			
		} else {	// zdp 
			DBG( "ZDP Frame has not been implemented." );
		} // if profileID
	} // if API_RAW

	/* find cb in 2 lists */
	// first in tmp list
//	GeneralCallbackCmp cmp1( CMPBYALL, &addr, profileID, clusterID, srcep, dstep, seqNO );
	pcb = genCbTmpList.find_remove( GeneralCallbackCmp(CMPBYALL, &addr, profileID, 
			clusterID, srcep, dstep, pFrame->seqNO()) );
	if( !pcb ) {
		//!!?? I still have to make sure that based on what kind of criterion for creating
		// pernament callbacks, just profileID and clusterID??		
		GeneralCallbackList::iterator it = find_if( genCbList.begin(), genCbList.end(), 
				GeneralCallbackCmp((ZigbeeCmpType_t)(CMPBYPROFILE | CMPBYCLUSTER), NULL, profileID, clusterID) );
		if( it != genCbList.end() )
			pcb = *it;
	}

	if( !pcb ) {
		DBG( "No callback processor found." );
		return -1;
	} else {
		CallbackProcessorFunc processor = pcb->routinePtr();
		if( processor )
			ret = processor( pFrame.release(), pcb );
	}
	
	return ret;
}

static
void* working_routine( void *arg )
{
	char *word;
	ssize_t n;

	DBG( "working_routine: thread_id = %u", (uint32)pthread_self() );

	while( running ) {
		word = read_a_word( zigbee_fd, readbuf, 0x02 );
		if( !word ) {
			DBG( "working_routine read error." );
			continue;
		}
		if( (char*)0x02 == word )
			parseGenRsp();
		else
			parseAtCmdRsp( word );
	} // while
	
	return 0;
}

static inline
int getDeviceList()
{
	return zigbee_at_cmd( API_ATCMD, NULL, "atdump 2", ZIGBEE_TIMEOUT );
}

static
int getDeviceInfo()
{
	int ret;
	void *vptr;
	ret = zigbee_at_cmd( API_ATCMD, NULL, "atinfo", ZIGBEE_TIMEOUT, &vptr );
	if( ret ) return ret;
	ZigbeeDevice *pDev = (ZigbeeDevice*)vptr;

//	DBG( "getDeviceInfo called." );
	if( pDev ) {
		thisDev = *pDev;
//#ifdef _DEBUG
//		pDev->printThisDevice();
//#endif
	} else 
		DBG( "get device info null ptr" );

	delete pDev;

	return 0;
}

int zigbee_init( char *dev_name, int baudrate, int flow_ctrl, 
			int databits, int stopbits, int parity )
{
	int fd, ret;

	DBG( "zigbee_init %s", dev_name );

	fd = OpenSerial(fd, dev_name);
	if( fd < 0 ) {
		ret = fd;
		perror( "Open device fail." );
		goto FAIL;
	}

	ret = InitSerial(fd, baudrate, flow_ctrl, databits, stopbits, parity);
	if( ret ) {
		perror( "InitSerial error!" );
		goto FAIL;
	}

	zigbee_fd = fd;
	
	//!! old way of at cmd test can be added here

//	zigbee_at_cmd( API_ATCMD, NULL, "atdump 2" );

//	DBG( "Reading all..." );
//	read_all(fd);

	// end of old test
	
	/* init global table and list for at command and general commands */
	// at commands
	atCmdCallbackTable.clear();
	atCmdCallbackTable["atinfo"] = new AtCmdCallback( "EINFO", parseAtInfo, 1 );
	atCmdCallbackTable["atieee"] = new AtCmdCallback( "EREPOIEEE", parseAtIeee, 1 );
	atCmdCallbackTable["atdump"] = new AtCmdCallback( "Neighbor:", parseAtDump, 1 );
	atCmdSyncCallbackPtr = 0;

	atCmdCallbackList.clear();
	atCmdCallbackList.push_back( new AtCmdCallback("EJOINED", parseNewJoin ) );

	
	// general cmd list both pernament and tmp
	genCbList.clear();			//!! TODO what kind of callbacks we need to deal?
	genCbTmpList.clear();

	// init device list
	zigbeeDevList.clear();

	// create and run reading thread
	running = 1;
	ret = pthread_create( &routine_tid, NULL, working_routine, NULL );
	if( ret ) {
		DBG( "start thread error: %s", strerror(ret) );
		goto FAIL;
	}

	DBG( "working_routine started. thread id = %u", (uint32)pthread_self() );

	ret = getDeviceInfo();
	if( ret ) {
		DBG( "getDeviceInfo error, %d", ret );
		goto FAIL;
	}

	ret = getDeviceList();
	if( ret ) {
		DBG( "getDeviceList error, %d", ret );
		goto FAIL;
	}
//#ifdef _DEBUG
//	printDevList();
//#endif	
	
	return 0;
FAIL:
	close( fd );
	atCmdSyncCallbackPtr = 0;
	running = 0;
	return ret;
}

int zigbee_finalize()
{
	DBG( "zigbee_finalize" );

	running = 0;
	// issue a comand to generate feedback output in order to prevent working routine blocked in read
	zigbee_at_cmd( API_ATCMD, NULL, "atinfo" );
	pthread_join( routine_tid, NULL );
	DBG( "working routine terminated." );
	
	close( zigbee_fd );

	/* delete all structures */
	// atCmdCallbackTable
	atCmdSyncCallbackPtr = 0;
	for( AtCmdCallbackTable::iterator it = atCmdCallbackTable.begin(); 
			it != atCmdCallbackTable.end(); ++it ) {
		CallbackPtr p = it->second;
		p->finish( Callback::SUCCESS, 0 );
		delete p;
	}
	atCmdCallbackTable.clear();
	atCmdCallbackList.clear();

	// general cmd lists
	genCbTmpList.setLock();
	for( GeneralCallbackSafeList::Iterator it = genCbTmpList.begin();
			it != genCbTmpList.end(); ++it ) {
		CallbackPtr p = *it;
		p->finish( Callback::SUCCESS, 0 );
		delete p;
	}
	genCbTmpList.releaseLock();

	for( GeneralCallbackList::iterator it = genCbList.begin();
			it != genCbList.end(); ++it ) {
		delete *it;
	}

	// clear device list
	zigbeeDevList.setLock();
	for( ZigbeeDeviceList::iterator it = zigbeeDevList.begin(); 
			it != zigbeeDevList.end(); ++it )
		delete *it;
	zigbeeDevList.clear();
	zigbeeDevList.releaseLock();
	
	return 0;
}

//static
//int Memcpy( uint8 **buf, uint8 **ptr, int *bufLen,
//			  void *data, int dataLen, float ratio )
//{
//	int offset = *ptr - *buf;

//	if( offset + dataLen > *bufLen ) {
////		DBG( "realloc" );
//		*bufLen = ratio * (offset + dataLen);
//		*buf = (uint8*)realloc( *buf, *bufLen );
//		if( !(*buf) )
//			return -1;
//		*ptr = *buf + offset;
//	} // if

//	memcpy( *ptr, data, dataLen );
//	*ptr += dataLen;

//	return 0;
//}


//void make_address( Address_t *addr, AddrType_t addrType, ... )
//{
//	va_list ap;
//	void*	pLongAddr;

//	memset( addr, 0, sizeof(*addr) );
//	addr->type = addrType;
//	if( NOTPRESENT == addrType )
//		return;

//	va_start( ap, addrType );
//	if( EXTENDED == addrType ) {
//		pLongAddr = va_arg( ap, void* );
//		memcpy( addr->addrData.ieeeAddr, pLongAddr, 8 );
//	} else {
//		addr->addrData.shortAddr = (uint16)va_arg( ap, int );
//	}
//	va_end(ap);

//	return;
//}

// dstAddr can be NULL refer to NOTPRESENT
int zigbee_send_data( CmdType_t cmdType, Address_t *dstAddress, const void *data, uint16 dataLen )
{
	int ret = 0;
	uint8 cmdId, *buf, *ptr;
	AddrType_t addrType;
	uint16 length;
	ssize_t n;

	DBG( "zigbee_send_data" );
//	pr_mem( data, dataLen );

	buf = (uint8*)malloc( dataLen + 14 );
	if( !buf ) {
		DBG( "memory allocation failed." );
		ret = -1;
		goto RET;
	}
	ptr = buf;

	*ptr = 0x02;	// start
	ptr += 3;		// skip length, compute it later
	// get cmdId
	addrType = dstAddress ? dstAddress->type : NOTPRESENT;
	cmdId = (uint8)( (uint8)addrType << 6 | (uint8)cmdType );
//	DBG( "CmdId = %02x", cmdId );
	*ptr++ = cmdId;
	// address
	switch( addrType ) {
	case NOTPRESENT:
		break;
	case EXTENDED:
		memcpy( ptr, dstAddress->addrData.ieeeAddr, 8 );
		ptr += 8;
		break;
	default:		// short addr
		{
		uint16 shortAddr = dstAddress->addrData.shortAddr;
#ifdef HOST_ZDEV_ENDIAN_DIFF
		BYTESWAP( &shortAddr, uint16 );
#endif
		memcpy( ptr, &shortAddr, 2 );
		ptr += 2;
		}
	} // switch 
	// data
	memcpy( ptr, data, dataLen );
	ptr += dataLen;
	// compute length
	length = (uint16)(ptr - buf - 3);
#ifdef HOST_ZDEV_ENDIAN_DIFF
	BYTESWAP( &length, uint16 );
#endif
	memcpy( buf+1, &length, 2 );
	// checksum
	*ptr = get_checksum( buf + 1, ptr );		// checksum
	*++ptr = 0x03;		// end
	++ptr;

	// write to device
	n = (ssize_t)(ptr - buf);
	pr_mem( buf, n );
	if( writen( zigbee_fd, buf, n ) != n ) {
		DBG( "write device error, %d", errno );
		ret = -1;
		goto RET;
	}

RET:
	free(buf);
	return ret;
}

int zigbee_at_cmd( CmdType_t cmdType, Address_t *dstAddress, 
		const char *cmdString, int timeout, void **result, uint16 *resultLen )
{
	int 		ret = 0;
	const char	*ptr;
	uint16		cbDataLen;
	
	using namespace std;

	DBG( "zigbee_at_cmd: %s, timeout=%d", cmdString, timeout );

	if( timeout ) {
		ptr = strpbrk( cmdString, " \t\f\r\v\n" );
		if( !ptr ) ptr = cmdString + strlen(cmdString);

		AtCmdCallbackTable::iterator it = atCmdCallbackTable.find( string(cmdString, ptr) );
		if( atCmdCallbackTable.end() != it )
			atCmdSyncCallbackPtr = it->second;
#ifdef _DEBUG
		else
			DBG( "Command %s has no processor.", cmdString );
#endif
	} // if timeout

//#ifdef _DEBUG
//	if( atCmdSyncCallbackPtr )
//		DBG( "zigbee_at_cmd atCmdSyncCallbackPtr->NAME is %s", atCmdSyncCallbackPtr->name() );
//	else		
//		DBG( "atCmdSyncCallbackPtr is NULL" );
//#endif
	ret = zigbee_send_data(cmdType, dstAddress, cmdString, strlen(cmdString) );
	if( ret ) {
		DBG( "zigbee_send_data error: %d", ret );
		goto RET;
	}

	// wait for finish
	if( timeout && atCmdSyncCallbackPtr ) {
		DBG( "Wait for finish %s.", atCmdSyncCallbackPtr->name() );
		ret = atCmdSyncCallbackPtr->wait( timeout );
		if( ret ) {
#ifdef _DEBUG			
			if( ETIMEDOUT == errno )
				DBG( "zigbee_at_cmd wait timeout." );
			else
				DBG( "zigbee_at_cmd wait error, %d", ret );
#endif			
			goto RET;
		} else {
			// on success, feedback the data
			DBG( "%s return success.", atCmdSyncCallbackPtr->name() );
			if( result )
				cbDataLen = atCmdSyncCallbackPtr->getCbData( *result );			
			if( resultLen )
				*resultLen = cbDataLen;
		} // if ret
	} // if

	
RET:
	atCmdSyncCallbackPtr = 0;
	return ret;
}

// write zcl frame to buf, which is pre-allocated by the caller
// return the length of the zcl frame on success
static
int writeZcl2Buf( uint8 control, uint8 cmd, const void *payload, 
		uint16 payloadLen, void *buf, uint8 seqNO, uint16 manufacturerCode )
{
	uint8 *ptr = (uint8*)buf;

	*ptr++ = control;
	// manufacturerCode
	if( control & ZCL_FRAME_MANU_SPEC ) {
#ifdef HOST_ZCL_ENDIAN_DIFF
		BYTESWAP( &manufacturerCode, uint16 );
#endif
		memcpy( ptr, &manufacturerCode, 2 );
		ptr += 2;
	} // if
	*ptr++ = seqNO;
	*ptr++ = cmd;
	// payload can be NULL
	if( payload ) {
		memcpy( ptr, payload, payloadLen );
		ptr += payloadLen;
	}
			
	return (int)( ptr - (uint8*)buf );
}


// sending general frame that containing profileID, clusterID and endpoints
// zcl and zdp
static
int general_request( Address_t *dstAddress, uint8 srcep, uint8 dstep, 
		uint16 profileID, uint16 clusterID, void *data, uint16 dataLen )
{
	int ret = 0;
	uint8 *buf, *ptr;

//	DBG( "general_request" );
//	pr_mem( zclData, dataLen );

	buf = (uint8*)malloc( dataLen + 6 );
	if( !buf ) {
		DBG( "memory allocation failed." );
		ret = -1;
		goto RET;
	}
	ptr = buf;

	// endpoints
	*ptr++ = srcep;
	*ptr++ = dstep;
	// profile and cluster IDs
#ifdef HOST_ZDEV_ENDIAN_DIFF
	BYTESWAP( &profileID, uint16 );
	BYTESWAP( &clusterID, uint16 );
#endif	
	memcpy( ptr, &profileID, 2 );
	ptr += 2;
	memcpy( ptr, &clusterID, 2 );
	ptr += 2;
	memcpy( ptr, data, dataLen );
	ptr += dataLen;			// data

	ret = zigbee_send_data( API_RAW, dstAddress, buf, dataLen + 6 );
	if( ret ) {
		DBG( "zigbee_send_data fail: %d", ret );
		goto RET;
	}

RET:	
	free( buf );
	return ret;
}

int zigbee_zcl_cmd( Address_t *dstAddress, uint8 srcep, uint8 dstep, 
		uint16 profileID, uint16 clusterID, const ZclFrame *pZclFrame, 
		int timeout, void **result, uint16 *resultLen )
{
	if( !pZclFrame )
		return 1;
	return zigbee_zcl_cmd( dstAddress, srcep, dstep, profileID, clusterID, 
				pZclFrame->frameCtrl(), pZclFrame->cmd(), pZclFrame->seqNO(), pZclFrame->payload(), 
				pZclFrame->payloadLen(), timeout, result, resultLen );
}

int zigbee_zcl_cmd( Address_t *dstAddress, uint8 srcep, uint8 dstep, 
		uint16 profileID, uint16 clusterID, uint8 zclControl, uint8 zclCmd, 
		uint8 zclSeqNO, const void *zclPayload, uint16 zclPayloadLen, 
		int timeout, void **result, uint16 *resultLen,
		uint16 zclManufacturerCode )
{
	uint8						*buf;
	int							ret = 0;
	int							zclFrameLen;
	GeneralCallbackPtr			pcb = 0;
	uint16						cbDataLen;

	using namespace std;

	if( timeout )
		zclControl &= ~ZCL_FRAME_NO_RSP;

	DBG( "zigbee_zcl_cmd, zcl frame is:" );

	buf = (uint8*)malloc( zclPayloadLen + 5 );
	if( !buf )
		goto FAIL;

	zclFrameLen = writeZcl2Buf( zclControl, zclCmd, zclPayload, zclPayloadLen, 
		buf, zclSeqNO, zclManufacturerCode );

	pr_mem( buf, zclFrameLen );

	// register callback
	//!! callback dstep is that of the device itself, srcep is that of the remote dev
	pcb = new GeneralCallback( dstAddress, profileID, 
			clusterID, dstep, srcep, zclSeqNO, parseZclCB, timeout != 0 );
	if( !pcb ) goto FAIL;
	genCbTmpList.add( pcb );

	ret = general_request( dstAddress, srcep, dstep, profileID, clusterID, buf, zclFrameLen );
	if( ret ) {
		DBG( "general_request error. %d", ret );
		goto FAIL;
	}
	
	if( timeout ) {
		// wait for finish
		ret = pcb->wait( timeout );
		if( ret ) {
			if( ETIMEDOUT == errno ) {
				DBG( "zigbee_zcl_cmd wait timeout" );
				//!! maybe working thread running in "finish"
				pcb->finish( Callback::FAIL, -1 );
			}
			DBG( "zcl request error, %d", ret );
			goto FAIL;
		} // if ret

		// success return, it's the caller's duty to delete
		DBG ( "zigbee_zcl_cmd success." );
		if( result )
			cbDataLen = pcb->getCbData( *result );		
		if( resultLen )
			*resultLen = cbDataLen;

		delete pcb;
	} // if  timeout
	
RET:
	free(buf);
	return ret;
FAIL:
	DBG( "zigbee_zcl_cmd fail: %s", strerror(errno) );
	
	free(buf);
	if( pcb ) {
		genCbTmpList.find_remove( bind2nd(equal_to<GeneralCallbackPtr>(), pcb) );
		delete pcb;
	}
	
	return -1;
}





#ifdef _DEBUG

static
void readAttrTest( int n )
{
	const uint16 	profileID 	= 0x0104;
	const uint16 	clusterID = 0x0; // basic cluster
	const uint8		srcEp = 0x0;
	const uint8		dstEp = 0x0;
	const uint16 	attrID	= (uint16)n;	// model id defined in basic server	
	const uint8		readAtrrReq = 0x0;
	const uint8		readAtrrRsp = 0x01;

	int 			ret;
	uint16 			addrData = 0x2ED7;
	Address_t 		addr;
	void 			*result;	
	ZclFrame 		*pResultFrame;
	uint16			rspAttrID;
	uint8			rspStatus;
	const uint8		*pRspData;
	uint8			rspDataType;

	DBG( "readAttrTest...." );
	
	make_address( &addr, SHORT, &addrData );

	ZclFrame reqFrame;
	reqFrame.setSeqNO( 1 );
	reqFrame.setCmd( readAtrrReq );
#ifdef HOST_ZCL_ENDIAN_DIFF
	BYTESWAP( &attrID, uint16 );
#endif
	reqFrame.setPayload( &attrID, 2 );

	ret = zigbee_zcl_cmd( &addr, srcEp, dstEp, profileID, clusterID, &reqFrame, 
			ZIGBEE_TIMEOUT, &result );

	if( ret ) {
		DBG( "zigbee_zcl_cmd fail, %d", ret );
		return;
	}
	if( !result ) {
		DBG( "callback returned null data." );
		return;
	}

	DBG( "readAttrTest got callback data." );
	pResultFrame = (ZclFrame*)result;
	pResultFrame->print();

	pRspData = pResultFrame->payload();
	rspAttrID = to_u16( pRspData );
	pRspData += 2;
#ifdef HOST_ZCL_ENDIAN_DIFF
	BYTESWAP( &rspAttrID, uint16 );
#endif
	if( rspAttrID != attrID )
		DBG( "Parse callback error." );
	rspStatus = *pRspData++;
	if( rspStatus )
		DBG( "Parse callback error." );
	rspDataType = *pRspData++;
	if( ZCL_DATATYPE_CHAR_STR == rspDataType ) {
		if( !isprint(*pRspData) )
			++pRspData;
		printf( "Model: %s\n", std::string((char*)pRspData, 
					(char*)(pResultFrame->payload()) + pResultFrame->payloadLen()).c_str() );
	} // if

	delete pResultFrame;
}


static
void switchTest()
{
	int ret;

	uint16 addrData = 0x2ED7;
	Address_t addr;
	void *result;	
	ZclFrame *pFrame;
	
	make_address( &addr, SHORT, &addrData );
	
	DBG( "switchTest" );

	ret = zigbee_zcl_cmd( &addr, 0x0, 0x0, 0x0104, 0x0006, 
			ZCL_FRAME_CLUSTER_SPEC, 0x02, 1, NULL, 0, ZIGBEE_TIMEOUT, &result );
	if( ret ) {
		DBG( "zigbee_zcl_cmd fail, %d", ret );
		return;
	}
	if( !result ) {
		DBG( "callback returned null data." );
		return;
	}

	DBG( "switchTest got callback data:" );
	pFrame = (ZclFrame*)result;
	pFrame->print();
	delete pFrame;
}

static
void atInfoTest()
{
	for( int i = 0; i < 10; ++i ) {
		DBG( "atInfoTest for the %d time.", i+1 );
		getDeviceInfo();
	}
}

static
void devTest()
{
	UbecPlug *pDev = new UbecPlug;
	pDev->setAddr( 0x2ED7 );
	pDev->toggle();
}

// testing
void testUtil()
{
	int			i;
	
	DBG( "testUtil..........." );

//	atInfoTest();
//	switchTest();

	// see zcl spec table 3.8
//	for( i = 0; i < 8; ++i )		
//		readAttrTest(i);

	devTest();

}


#endif	//_DEBUG








