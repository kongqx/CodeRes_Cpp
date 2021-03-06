#include "zigbee.h"

#ifdef _DEBUG
FILE *zigbeelog1;
FILE *zigbeelog2;
const char *zigbeelog1_filename;
const char *zigbeelog2_filename;
uint8 zigbee_role_type;
#endif


int 								zigbee_fd;

std::auto_ptr<ZigbeeDevice> 		thisDev;


// global DS
ZigbeeServiceSet zigbeeSrvSet;
ThreadSafePtrSet<ZigbeeDevicePtr, ZigbeeDeviceCompare> zigbeeDevSet;
//ThreadSafePtrSet<ZigbeeDevicePtr, ZigbeeDeviceCompare> zigbeeCandDevSet;


// event handler func
ZigbeeEventCallback eventHandler = NULL;

/* static global vars */
// thread id of working routine
static pthread_t                    routine_tid = 0;
// control the routine thread, initialized in zigbee_init()
static volatile uint8               running;


// for handling SRSP callback, globally unique, for only one cmd can be run at one time
//!! must Wait SRSP come back
static std::auto_ptr<GeneralCallback> 		cmdSRSPCallbackPtr;
// cmd callback pernament list
static CallbackSafeList						cmdCbTmpList;
// call back cmds and their parser
static CallbackParserTable							callbackParserTable;



// General callback pernament list, for dealing with notifications generated by the device
static CallbackList      			genCbList;
// General callback temporary list, for dealing responses of the zcl or zdp request
// the processor of callback will call its find_remove() to get a match cb.
// and if this cb isn't sync, the processor will delete it after job is done.
static CallbackSafeList      		genCbTmpList;
// for broadcast list
static CallbackSafeList				genBroadcastCbList;


// for Read and put_back_read
static char 		readBuf[ZIGBEE_BUFLEN + ZIGBEE_BUFLEN / 2];
static char 		*pReadStart = readBuf;
static char 		*pReadEnd = readBuf;

// for send cmd to zdev
static ZigbeePayload		serialPayload( ZIGBEE_BUFLEN );


// for send Data confirm AREQ callback
static int parseSendDataConfirm( Callback *pcb, void *arg1, void *arg2 );
static GeneralCallbackPtr sendConfirmCB( new AREQCallback(AF_DATA_CONFIRM, parseSendDataConfirm) );



#ifdef _DEBUG

inline static
void print_coord_info()
{
	DBG( "Zigbee Coord Info:" );
	DBG( "State:\t\t\t\t%d", thisDev->getState() );
	DBG2( "MacAddr:\t\t\t\t" );
	pr_mem( thisDev->getAddr().getMacAddr(), 8 );
	DBG( "NwkAddr:\t\t\t\t%04x", thisDev->getAddr().getNwkAddr() );		
	DBG( "Channel:\t\t\t\t%d", thisDev->getChannel() );
	DBG( "PANID:\t\t\t\t%04x", thisDev->getPanID().getNwkAddr() );	
	DBG2( "extPANID:\t\t\t\t" );
	pr_mem( thisDev->getPanID().getMacAddr(), 8 );	
}

#endif


//!! free cb data, normally it's the user who waiting for a callback's return to free its cbData
// mem, but once wait fail like timeout, the cb obj itself must free those mem in its destructor
// pass these function ptr to the CB obj by its setCbData()
//!! typname T must be a pointer.
template < typename T >
static void deleteCbData( void *vptr, bool isArray )
{
	T *ptr = (T*)vptr;

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


ssize_t Read(int fd, void *vptr, size_t n)
{
	char				*ptr = (char*)vptr;
	ssize_t 			nRead;
	ssize_t				dataSize;

	dataSize = pReadEnd - pReadStart;
	
	if( dataSize > 0 ) {
		nRead = dataSize > n ? n : dataSize;
		memcpy( ptr, pReadStart, nRead );
		pReadStart += nRead;
		return nRead;
	} 
	
	pReadStart = pReadEnd = readBuf;
	nRead = read( fd, readBuf, ZIGBEE_BUFLEN );
	if( nRead <= 0 )
		return nRead;
	pReadEnd += nRead;	
	pr_mem2( readBuf, nRead );
	if( nRead > n )
		nRead = n;
	memcpy( ptr, pReadStart, nRead );
	pReadStart += nRead;

	return nRead;
}



ssize_t	readn(int fd, void *vptr, size_t n)
{
	size_t	nleft;
	ssize_t	nread;
	char	*ptr;

	ptr = (char*)vptr;
	nleft = n;
	while (nleft > 0) {
		if ( (nread = Read(fd, ptr, nleft)) < 0) {
			if (errno == EINTR)
				nread = 0;		/* and call read() again */
			else
				return(-1);
		} else if (nread == 0)
			break;				/* EOF */

		nleft -= nread;
		ptr   += nread;
	}

	return(n - nleft);		/* return >= 0 */
}



ssize_t writen(int fd, const void *ptr, size_t n)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	size_t		nleft;
	ssize_t		nwritten;

	pthread_mutex_lock( &lock );
	nleft = n;
	while (nleft > 0) {
		if ((nwritten = write(fd, ptr, nleft)) < 0) {
			if (nleft == n)
				goto FAIL;
			else
				break;      /* error, return amount written so far */
		} else if (nwritten == 0) {
			break;
		}
		nleft -= nwritten;
        ptr = (char*)ptr + nwritten;
	}
	pthread_mutex_unlock( &lock );
	return(n - nleft);      /* return >= 0 */
FAIL:
	pthread_mutex_unlock( &lock );
	return -1;
}



void zigbee_register_event_callback( ZigbeeEventCallback callbackFunc )
{
    eventHandler = callbackFunc;
}


void zigbee_get_dev_list( std::vector<ZigbeeDevicePtr> &devlist )
{
//	zigbeeDevList.getCopy( devlist );	
}


static inline
void clear_all()
{
	routine_tid = 0;
	cmdSRSPCallbackPtr.reset( NULL );
	cmdCbTmpList.clear();
	callbackParserTable.clear();
	genCbList.clear();
	genCbTmpList.clear();
	genBroadcastCbList.clear();
	thisDev.reset( NULL );

	zigbeeSrvSet.clear();
	zigbeeDevSet.clear();
//	zigbeeCandDevSet.clear();
}


static 
void put_back_read( const uint8 *pStart, const uint8 *pEnd )
{
	ssize_t		dataSize, spaceSize, diff, copySize;

	if( pStart >= pEnd )
		return;

	dataSize = pReadEnd - pReadStart;
	copySize = pEnd - pStart;

	if( !dataSize ) {
		pReadStart = pReadEnd = readBuf;
		memcpy( readBuf, pStart, copySize );
		pReadEnd += copySize;
		return;
	}

	spaceSize = pReadStart - readBuf;
	diff = copySize - spaceSize;
	if( diff > 0 ) {
		memmove( pReadStart + diff, pReadStart, dataSize );
		pReadStart = readBuf;
	} else {
		pReadStart -= copySize;
	}
	memcpy( pReadStart, pStart, copySize );
}


static
uint8 get_checksum( const uint8 *pStart, const uint8 *pEnd )
{
	uint8 checksum = 0;
//	DBG( "get_checksum()" );
//	pr_mem( pStart, pEnd - pStart );
	while( pStart != pEnd )
		checksum ^= *pStart++;
	return checksum;
}


static
int do_zdev_cmd( uint16 cmdNO, uint16 SRSPCmdNO, const void *cmdData, uint8 cmdDataLen )
{
	uint8						checksum;

	DBG( "do_at_cmd() %04x", cmdNO );

	serialPayload.clear();
	
	serialPayload.appendData( 0xFE ); // SOF
	serialPayload.appendData( cmdDataLen );
#ifdef HOST_ZDEV_ENDIAN_DIFF
	BYTESWAP( &cmdNO, uint16 );
#endif
	serialPayload.appendData( &cmdNO, 2 );		// cmd
	// some cmd without any data
	if( cmdData )
		serialPayload.appendData( cmdData, cmdDataLen );

	checksum = get_checksum( serialPayload.data()+1, serialPayload.data()+serialPayload.dataLen() );
	serialPayload.appendData( checksum );

	pr_mem( serialPayload.data(), serialPayload.dataLen() );

	// some cmds like Reset only have AREQ callback
	if( SRSPCmdNO ) {
		if( !(cmdSRSPCallbackPtr.get()) || cmdSRSPCallbackPtr->getCmdNO() != SRSPCmdNO )
			cmdSRSPCallbackPtr.reset( new GeneralCallback(SRSPCmdNO) );
		else
			cmdSRSPCallbackPtr->reset();
	} // if SRSPCmdNO

	if( writen( zigbee_fd, serialPayload.data(), serialPayload.dataLen() ) != serialPayload.dataLen() ) {
		DBG( "Write to SerialPort error: %s", strerror(errno) );
		return -1;
	}

	if( SRSPCmdNO ) {
		if( cmdSRSPCallbackPtr->wait() ) {
			DBG( "SRSP Callback wait error: %s", strerror(errno) );
			return -1;
		}
	} // if SRSPCmdNO

	return 0;
}


static
uint8 *read_a_frame( int fd )
{
	static uint8 		buf[ZIGBEE_BUFLEN];
	uint8 				*ptr = buf;
	uint8 				data, length, checksum;
	ssize_t				nRead;

//	DBG( "read_a_frame()" );

	while( readn( fd, &data, 1 ) == 1 ) {
		if( 0xFE == data ) {
			*ptr++ = data;
			if( readn( fd, ptr, 1 ) != 1 )
				goto FAIL;
			length = *ptr++;
			if( (nRead = readn( fd, ptr, length + 3 )) < 0 ) {
				goto FAIL;
			} else {
//				pr_mem( ptr, nRead );
				ptr += nRead;
			}
			if( nRead != length + 3 )
				goto FAIL;
//			DBG( "get_checksum %02x == %02x", get_checksum(buf + 1, ptr - 1), *(ptr - 1) );
			if( get_checksum(buf + 1, ptr - 1) != *(ptr - 1) )
				goto FAIL;
			return buf;
		} // if
	} // while

FAIL:
	put_back_read( buf + 1, ptr );
	return NULL;
}


static
void parseIncommingData( const uint8 *pStart, const uint8 *pEnd )
{
	DBG( "parseIncommingData() TODO" );
	// should find cb in a dedicated list
	pr_mem( pStart, (pEnd - pStart) );
}



static
void handleSRSP( uint16 cmdNO, const uint8 *pStart, const uint8 *pEnd )
{
//	DBG( "handleSRSP() %04x", cmdNO );

	if( cmdSRSPCallbackPtr.get() && cmdNO == cmdSRSPCallbackPtr->getCmdNO() ) {
		// Leave the task of analying feedback data to those funcs like read_nv_config
		cmdSRSPCallbackPtr->setCbData( new ZigbeePayload(pStart, pEnd), deleteCbData<ZigbeePayload> );
		cmdSRSPCallbackPtr->finish( Callback::SUCCESS, 0 );		// seems always to be success
	} else if( INVALID_CMD == cmdNO ) {
		DBG( "Invalid command!" );
		cmdSRSPCallbackPtr->setCbData( new ZigbeePayload(pStart, pEnd), deleteCbData<ZigbeePayload> );
		cmdSRSPCallbackPtr->finish( Callback::FAIL, 1 );		// seems always to be success
	}
#ifdef _DEBUG
	else {
		DBG( "Unknown SRSP CMD %04x", cmdNO );
	}
#endif

	return;
}


// for handling all AREQ cmds except AF_INCO
//static
//int parseGenCBData( uint16 cmdNO, const uint8 *pStart, const uint8 *pEnd )
//{
//	pcb->setCbData( new ZigbeePayload(pStart, pEnd), deleteCbData<ZigbeePayload> );
//	pcb->finish( Callback::SUCCESS, 0 );
//	return 0;
//}

inline static
int parseStateChange( Callback *pcb, void *arg1, void *arg2 )
{
	uint8 *pStart = (uint8*)arg1;
	uint8 *pEnd = (uint8*)arg2;

	DBG( "received state change signal, state is %02x", *pStart );

	return 0;
}


inline static
int parseNewJoin( Callback *pcb, void *arg1, void *arg2 )
{
	uint8 					*pStart = (uint8*)arg1;
	uint8 					*pEnd = (uint8*)arg2;
	uint16					nwkAddr;
	ZigbeeDevicePtr 		pDev( new ZigbeeDevice );

	DBG( "parseNewJoin() called" );
	pStart += 2;	
	nwkAddr = to_u16(pStart);
	pDev->setAddr( nwkAddr );
	pStart += 2;
	pDev->setAddr( pStart );
	pStart += 8;
//	DBG( "Dev Capabilites %02x", *pStart );
	zigbeeDevSet.add( pDev );

#ifdef _DEBUG
	DBG( "%s Joined this network.", pDev->toString() );
#endif

	return 0;
}


inline static
int parseSendDataConfirm( Callback *pcb, void *arg1, void *arg2 )
{
	uint8 *pStart = (uint8*)arg1;
	uint8 *pEnd = (uint8*)arg2;
	GeneralCallback *gcb = dynamic_cast<GeneralCallback*>(pcb);
	uint8 status = *pStart++;
	uint8 ep = *pStart++;
	uint8 seq = *pStart;
	int ret = 1;

	if( ep == gcb->getSrcEp() && seq == gcb->getSeqNO() ) {
		if( ret = status )
			gcb->finish( Callback::FAIL, ret );
		else
			gcb->finish( Callback::SUCCESS, 0 );
	} //if
	
	return ret;
}



inline static
int genAREQHandler( Callback *pcb, void *arg1, void *arg2 )
{
	uint8 *pStart = (uint8*)arg1;
	uint8 *pEnd = (uint8*)arg2;
	pcb->setCbData( new ZigbeePayload(pStart, pEnd), deleteCbData<ZigbeePayload> );
	pcb->finish( Callback::SUCCESS, 0 );
	return 0;
}


static
void* working_routine( void *arg )
{
	uint8 		*pFrame, *pStart, *pEnd;
	uint8		length;
	uint16		cmdNO;

	DBG( "working_routine: thread_id = %lu", pthread_self() );

	while( running ) {
		pFrame = read_a_frame( zigbee_fd );
		if( !pFrame ) {
			DBG( "working_routine read error." );
			continue;
		}

		pStart = pFrame + 1;
		length = *pStart++;
		cmdNO = to_u16( pStart );
#ifdef HOST_ZDEV_ENDIAN_DIFF
		BYTESWAP( &cmdNO, uint16 );
#endif
		pStart += 2;
		pEnd = pStart + length;

		// pStart and pEnd contain data seg
		if( AF_INCOMING_MSG == cmdNO ) {		// special
			parseIncommingData( pStart, pEnd );
		} else if( AF_DATA_CONFIRM == cmdNO ) {
			sendConfirmCB->runProcessor( pStart, pEnd );
		} else {
			// check cmd type
			ZigbeeCmdType cmdType = (ZigbeeCmdType)((cmdNO >> 5) & 0x07);
			if( ZIGBEE_CMD_SRSP == cmdType ) {
				handleSRSP( cmdNO, pStart, pEnd );
			} else if( ZIGBEE_CMD_AREQ == cmdType ) {
				CallbackParserTable::iterator it = callbackParserTable.find( cmdNO );
				if( it != callbackParserTable.end() ) {
					GeneralCallbackPtr pcb = it->second;
					pcb->runProcessor( pStart, pEnd );
//					const CallbackProcessorFunc processor = pcb->getProcessor();
//					if( processor )
//						processor( pcb.get(), pStart, pEnd );
				}
#ifdef _DEBUG
				else {
					DBG( "No AREQ callback for cmd %04x", cmdNO );
				}
#endif
			} // if cmdType
		} // if cmdNO
	} // while
	
	return 0;
}



static
int write_nv_config( uint16 configid, const void *pValue, uint8 valueLen )
{
	std::auto_ptr<ZigbeePayload> 		result;
	int									ret;
	const uint8							*ptr;
	ZigbeePayload 						buf( valueLen + 4 );

	DBG( "write_nv_config %04x", configid );

#ifdef HOST_ZDEV_ENDIAN_DIFF
	BYTESWAP( &configid, uint16 );
#endif
	buf.appendData( &configid, 2 );
	buf.appendData( 0 );		// offset
	buf.appendData( valueLen );
	buf.appendData( pValue, valueLen );

	if( (ret = do_zdev_cmd(SYS_OSAL_NV_WRITE, SYS_OSAL_NV_WRITE_SRSP, buf.data(), buf.dataLen())) ) {
		DBG( "do_zdev_cmd error" );
		return ret;
	}

	result.reset( (ZigbeePayload*)(cmdSRSPCallbackPtr->getCbData()) );
	ptr = result->data();
	ret = *ptr;
	if( ret ) {
		DBG( "write_nv_config() got error status %d", ret );
		return ret;
	}
	
	return 0;
}


// pValue & pValueLen must not be NULL when calling
// NOTE! returned data kepet in pValue is in the Endian of ZDEV
static
int read_nv_config( uint16 configid, void *pValue, uint8 *pValueLen )
{
	uint8								buf[3];
	std::auto_ptr<ZigbeePayload> 		result;
	int									ret;
	const uint8							*ptr;

	DBG( "read_nv_config() %04x", configid );

#ifdef HOST_ZDEV_ENDIAN_DIFF
	BYTESWAP( &configid, uint16 );
#endif
	memcpy( buf, &configid, 2 );
	buf[2] = 0;			// offset always to be 0

	if( (ret = do_zdev_cmd(SYS_OSAL_NV_READ, SYS_OSAL_NV_READ_SRSP, buf, 3)) ) {
		DBG( "do_zdev_cmd error" );
		return ret;
	}

	result.reset( (ZigbeePayload*)(cmdSRSPCallbackPtr->getCbData()) );
	ptr = result->data();
	// read status
	ret = *ptr++;
	if( ret ) {
		DBG( "read_nv_config() got error status %d", ret );
		return ret;
	}

	*pValueLen = *ptr++;
	memcpy( pValue, ptr, *pValueLen );
//	pr_mem( ptr, *pValueLen );

	return 0;
}



static
int check_nv_config( uint16 configid, const void *pValue, uint8 valueLen, bool &change )
{	
	uint8 					length;
	int						ret;
	char					buf[ZIGBEE_BUFLEN];

	DBG( "check_nv_config() %04x", configid );

	if( (ret = read_nv_config(configid, buf, &length)) ) {
		DBG("read_nv_config error %d", ret);
		return ret;
	}

	// compare data
	if( length == valueLen && memcmp(buf, pValue, length) == 0 ) {
		DBG( "check success!" );
		change = false;
		return 0;
	}

	change = true;
	
	DBG( "Original value:" );
	pr_mem( buf, length );
	DBG( "NOT Equal to desired value:" );
	pr_mem( pValue, valueLen );
	
	if( (ret = write_nv_config(configid, pValue, valueLen)) ) {
		DBG("write_nv_config error %d", ret);
		return ret;
	}

	return 0;
}


static
int zigbee_reset()
{
	int 			ret;
	uint8			type = 1;

	DBG( "zigbee_reset()" );

	GeneralCallbackPtr pcb = callbackParserTable.find( SYS_RESET_IND )->second;
	pcb->reset();

	// no srsp feedback
	ret = do_zdev_cmd( SYS_RESET_REQ, 0, &type, 1 );
	if( ret ) {
		DBG( "zigbee_reset() send cmd error." );
		return ret;
	}

	ret = pcb->wait();
	if( ret ) {
		DBG( "zigbee_reset() wait error." );
		return ret;
	}

	std::auto_ptr<ZigbeePayload> pData( (ZigbeePayload*)(pcb->getCbData()) );

//	DBG( "zigbee_reset() has got this response: " );
//	pr_mem( pData->data(), pData->dataLen() );

	return *(pData->data()) == 0 ? 0 : -1;
}


inline static
int get_coord_info()
{
	uint8			item;
	int				ret;
	std::auto_ptr<ZigbeePayload>		result;
	const uint8 						*ptr;	
	
	for( item = 0; item < 8; ++item ) {
//		DBG( "get_coord_info() %d", item );
		
		if( (ret = do_zdev_cmd(ZB_GET_DEVICE_INFO, ZB_GET_DEVICE_INFO_SRSP, &item, 1)) ) {
			DBG( "do_zdev_cmd error" );
			return ret;
		} // if

		result.reset( (ZigbeePayload*)(cmdSRSPCallbackPtr->getCbData()) );
		ptr = result->data();		

		switch( *ptr++ ) {
		case 0:
			thisDev->setState( *ptr );
//			DBG( "state = %d", *ptr );
			break;
		case 1:
			// mac addr
			thisDev->setAddr( ptr );
//			DBG( "mac addr = " );
//			pr_mem( ptr, 8 );
			break;
		case 2:
			thisDev->setAddr( to_u16(ptr) );
//			DBG( "nwkaddr = %04x", to_u16(ptr) );
			break;
//		case 3:
//			thisDev->getParent()->setAddr( to_u16(ptr) );
//			break;
//		case 4:
//			thisDev->getParent()->setAddr( ptr );
//			break;
		case 5:
			thisDev->setChannel( *ptr );
			break;
		case 6:
			thisDev->setPanID( to_u16(ptr) );
//			DBG( "PANID = %04x", to_u16(ptr) );
			break;
		case 7:
			thisDev->setPanID( ptr );
//			DBG( "extPANID = " );
//			pr_mem( ptr, 8 );			
			break;
		} // switch
	} // for

	return 0;
}


//!! ZNwkInvalidRequest 0xC2
int zigbee_scan()
{
	static const uint32					channel = 0x00100000;
	static const uint8					duration = 3;
	int									ret;
	std::auto_ptr<ZigbeePayload> 		result;
	const uint8							*ptr;
	ZigbeePayload						cmdData(5);

	DBG( "zigbee_scan()" );
	
	cmdData.appendData( &channel, 4 );
	cmdData.appendData( duration );

	if( (ret = do_zdev_cmd(ZDO_NWK_DISCOVERY_REQ, ZDO_NWK_DISCOVERY_REQ_SRSP, cmdData.data(), cmdData.dataLen())) ) {
		DBG( "do_zdev_cmd error" );
		return ret;
	}

	result.reset( (ZigbeePayload*)(cmdSRSPCallbackPtr->getCbData()) );
	ptr = result->data();
	ret = *ptr;

	DBG( "zigbee_scan() finish with retcode = %d", ret );

	return ret;	
}


//!! fail
int zigbee_set_link_key()
{
	static const uint8 keyData[] = { 0x5A, 0x69, 0x67, 0x42, 0x65, 0x65, 0x41, 0x6C, 0x6C, 0x69, 0x61, 0x6E, 0x63, 0x65, 0x30, 0x39 };
	int			ret;
	ZigbeePayload						cmdData(26);
	std::auto_ptr<ZigbeePayload> 		result;
	const uint8							*ptr;
	uint16								nwkAddr;

	DBG( "zigbee_set_link_key()" );

	nwkAddr = thisDev->getAddr().getNwkAddr();
	cmdData.appendData( &nwkAddr, 2 );
	cmdData.appendData( thisDev->getAddr().getMacAddr(), 8 );
	cmdData.appendData( keyData, sizeof(keyData) );

	if( (ret = do_zdev_cmd(ZDO_SET_LINK_KEY, ZDO_SET_LINK_KEY_SRSP, cmdData.data(), cmdData.dataLen())) ) {
		DBG( "do_zdev_cmd error" );
		return ret;
	}

	result.reset( (ZigbeePayload*)(cmdSRSPCallbackPtr->getCbData()) );
	ptr = result->data();
	ret = *ptr;

	return ret;
}


static
int zigbee_send_data( uint16 dstAddr, uint8 dstEp, uint8 srcEp, uint16 clusterID, 
		uint8 transID, const void *data, uint8 dataLen, uint8 opt = 0, uint8 radius = 30 )
{
	int 								ret;
	ZigbeePayload						cmdData(dataLen + 10);
	std::auto_ptr<ZigbeePayload> 		result;
	const uint8							*ptr;

	DBG( "zigbee_send_data() to %04x:%02x", dstAddr, dstEp );

#ifdef HOST_ZDEV_ENDIAN_DIFF
	BYTESWAP( &dstAddr, uint16 );
#endif
	cmdData.appendData( &dstAddr, 2 );
	cmdData.appendData( dstEp );
	cmdData.appendData( srcEp );
#ifdef HOST_ZDEV_ENDIAN_DIFF
	BYTESWAP( &clusterID, uint16 );
#endif
	cmdData.appendData( &clusterID, 2 );
	cmdData.appendData( transID );
	cmdData.appendData( opt );
	cmdData.appendData( radius );
	cmdData.appendData( dataLen );
	cmdData.appendData( data, dataLen );

	// prepare data confirm callback
	sendConfirmCB->reset();
	sendConfirmCB->setSrcEp( srcEp );
	sendConfirmCB->setSeqNO( transID );

	if( (ret = do_zdev_cmd(AF_DATA_REQUEST, AF_DATA_REQUEST_SRSP, cmdData.data(), cmdData.dataLen())) ) {
		DBG( "do_zdev_cmd() error" );
		return ret;
	} // if

	result.reset( (ZigbeePayload*)(cmdSRSPCallbackPtr->getCbData()) );
	ptr = result->data();	
	if( ret = *ptr ) {
		DBG( "SRSP status error %d", ret );
		return ret;
	}

	if( ret = sendConfirmCB->wait() ) {
		DBG( "sendConfirmCB error %d", ret );
		return ret;
	}

	return 0;
}


static
int zigbee_register_app( uint8 ep, uint16 profileID, uint16 devID, uint8 devVer, 
			uint8 nInClusters, const uint16 *inClustersList, uint8 nOutClusters, const uint16 *outClusterList )
{
	int 								ret;
	uint16								clusterID;
	ZigbeePayload						cmdData(0x49);
	std::auto_ptr<ZigbeePayload> 		result;
	const uint8							*ptr;

	DBG( "zigbee_register_app() %d", ep );

	cmdData.appendData( ep );
#ifdef HOST_ZDEV_ENDIAN_DIFF
	BYTESWAP( &profileID, uint16 );
#endif
	cmdData.appendData( &profileID, 2 );
#ifdef HOST_ZDEV_ENDIAN_DIFF
	BYTESWAP( &devID, uint16 );
#endif
	cmdData.appendData( &devID, 2 );
	cmdData.appendData( devVer );
	cmdData.appendData( 0x0 );		// latency Req always 0
	cmdData.appendData( nInClusters );
	for( int i = 0; i < nInClusters; ++i ) {
		clusterID = *inClustersList++;
#ifdef HOST_ZDEV_ENDIAN_DIFF
		BYTESWAP( &clusterID, uint16 );
#endif
		cmdData.appendData( &clusterID, 2 );
	} // for
	cmdData.appendData( nOutClusters );
	for( int i = 0; i < nOutClusters; ++i ) {
		clusterID = *outClusterList++;
#ifdef HOST_ZDEV_ENDIAN_DIFF
		BYTESWAP( &clusterID, uint16 );
#endif
		cmdData.appendData( &clusterID, 2 );		
	} // for

	if( (ret = do_zdev_cmd(AF_REGISTER, AF_REGISTER_SRSP, cmdData.data(), cmdData.dataLen())) ) {
		DBG( "do_zdev_cmd() error" );
		return ret;
	} // if

	result.reset( (ZigbeePayload*)(cmdSRSPCallbackPtr->getCbData()) );
	ptr = result->data();	

	if( ret = *ptr ) {
		DBG( "zigbee_register_app() got invalid status %x", ret );
		return ret;
	}

	return 0;
}



static
int zigbee_start_cb( Callback *pcb, void *arg1, void *arg2 )
{
#ifdef ZIGBEE_COORDINATOR
	static const uint8 state = DEV_ZB_COORD;
#else
	static const uint8 state = DEV_ZB_ENDDEV;
#endif

	uint8 *pStart = (uint8*)arg1;
	uint8 *pEnd = (uint8*)arg2;

	if( *pStart == state ) {
		pcb->finish( Callback::SUCCESS, 0 );
		return 0;
	}

	return *pStart;
}


static
int zigbee_start( bool waitFlag = true )
{
	int ret;
	uint16 delay = 0;
	GeneralCallbackPtr pcb;
	CallbackProcessorFunc oldProcessor;

#ifndef ZIGBEE_COORDINATOR
	static const uint16 outClusters[] = { ZCL_CLUSTER_ID_GEN_ON_OFF };
#endif

	DBG( "zigbee_start() waitFlag = %s", waitFlag ? "true" : "false" );

	if( waitFlag ) {
#ifdef ZIGBEE_COORDINATOR	
		if( ret = zigbee_register_app(COORD_ENDPOINT, ZCL_HA_PROFILE_ID, ZCL_HA_DEVICEID_COMBINED_INETRFACE, DEVICEVERSION, 0, NULL, 0, NULL) ) {
			DBG( "zigbee_register_app() error %d", ret );
//			return ret;
		}
#else
		if( ret = zigbee_register_app(ENDDEV_ENDPOINT, ZCL_HA_PROFILE_ID, ZCL_HA_DEVICEID_ON_OFF_SWITCH, DEVICEVERSION, 0, NULL, ARRLEN(outClusters), outClusters) ) {
			DBG( "zigbee_register_app() error %d", ret );
//			return ret;
		}
#endif // ZIGBEE_COORDINATOR
	} // waitFlag

	// wait for state change to 9 or 6
	if( waitFlag ) {
		pcb = callbackParserTable.find( ZDO_STATE_CHANGE_IND )->second;
		oldProcessor = pcb->setProcessor( zigbee_start_cb );
		pcb->reset();
	}

#ifdef HOST_ZDEV_ENDIAN_DIFF
	BYTESWAP( &delay, uint16 );
#endif

	if( (ret = do_zdev_cmd(ZDO_STARTUP_FROM_APP, ZDO_STARTUP_FROM_APP_SRSP, &delay, 2)) ) {
		DBG( "do_zdev_cmd error" );
		return ret;
	}

	std::auto_ptr<ZigbeePayload> pResult( (ZigbeePayload*)(cmdSRSPCallbackPtr->getCbData()) );
	ret = *(pResult->data());
	if( ret && ret != 1 ) {
		DBG( "zigbee_start() invalid SRSP status %x", ret );
		return ret;
	}

//	DBG( "zigbee_start() retcode = %d", ret );

	if( waitFlag ) {
		if( ret = pcb->wait() ) {
			DBG( "wait for AREQ callback error." );
			return ret;
		}
		pcb->setProcessor( oldProcessor );
	}


	if( (ret = get_coord_info()) )
		return ret;

#ifdef _DEBUG
	print_coord_info();
#endif

//	DBG( "zigbee_start() finish!" );
	
	return 0;
}


inline static
void prepare_callback()
{
	DBG( "prepareCallback TODO" );
	/* here we must specify profileid, clusterid, and cmdid */
	// for dealing with auto reporting
//	genCbList.add( new GeneralCallback(NULL, ZCL_HA_PROFILE_ID, ZCL_CLUSTER_ID_GEN_BASIC, 0x0, 0x0, 0x0, 
//						handleAutoReport, 0, 0x0a) );
//	// for dealing with alarm
//	genCbList.add( new GeneralCallback(NULL, ZCL_HA_PROFILE_ID, ZCL_CLUSTER_ID_GEN_ALARMS, 0x0, 0x0, 0x0, 
//						handleAlarm, 0, 0x0) );


	callbackParserTable.insert( CallbackParserTable::value_type(SYS_RESET_IND, GeneralCallbackPtr(new AREQCallback(SYS_RESET_IND, genAREQHandler))) );
	callbackParserTable.insert( CallbackParserTable::value_type(ZDO_STATE_CHANGE_IND, GeneralCallbackPtr(new AREQCallback(ZDO_STATE_CHANGE_IND, parseStateChange))) );
	callbackParserTable.insert( CallbackParserTable::value_type(ZDO_END_DEVICE_ANNCE_IND, GeneralCallbackPtr(new AREQCallback(ZDO_END_DEVICE_ANNCE_IND, parseNewJoin))) );
	
	return;
}


static
int do_check_config( uint8 type, uint8 clearOpt, bool &change )
{
	uint16 tmp8;
	uint16 tmp16;
    uint32 tmp32;
	bool nvChange;

	change = false;
	
	if( check_nv_config( ZCD_NV_STARTUP_OPTION, &clearOpt, 1, nvChange ) )
		return -1;
	change |= nvChange;
	if( check_nv_config( ZCD_NV_LOGICAL_TYPE, &type, 1, nvChange ) )
		return -1;
	change |= nvChange;
	tmp16 = 0xFFFF;
	if( check_nv_config( ZCD_NV_PANID, &tmp16, 2, nvChange ) )
		return -1;
	change |= nvChange;
	tmp32 = 0x00100000;
	if( check_nv_config( ZCD_NV_CHANLIST, &tmp32, 4, nvChange ) )
		return -1;
	change |= nvChange;
	tmp8 = 1;
	if( check_nv_config( ZCD_NV_ZDO_DIRECT_CB, &tmp8, 1, nvChange ) )
		return -1;
	change |= nvChange;

	return 0;
}


int zigbee_init( char *dev_name, uint8 type, uint8 clearOpt, int baudrate, int flow_ctrl, 
			int databits, int stopbits, int parity )
{
	int fd, ret, i;
	bool configChange;
	bool hasReset = false;

#ifdef _DEBUG
	zigbeelog1 = fopen( zigbeelog1_filename, "w" );
	setbuf( zigbeelog1, NULL ); 
	zigbeelog2 = fopen( zigbeelog2_filename, "w" );
	setbuf( zigbeelog2, NULL ); 
#endif	
	DBG( "ZigWay SDK Version: %s", ZIGBEE_VERSION );
	DBG( "zigbee_init %s type = %d, clearOpt = %d", dev_name, type, clearOpt );
//	DBG( "ZIGBEE_LOG1 = %s", ZIGBEE_LOG1 );

	fd = OpenSerial(fd, dev_name);
	if( fd < 0 ) {
		ret = fd;
		DBG( "Cannot open device %s", dev_name );
		perror( "Open device fail." );
		goto FAIL;
	}

	ret = InitSerial(fd, baudrate, flow_ctrl, databits, stopbits, parity);
	if( ret ) {
		DBG( "InitSerial fail." );
		perror( "InitSerial error!" );
		goto FAIL;
	}

	zigbee_fd = fd;
	
	
	//!! old way of at cmd test can be added here

	// end of old test


	clear_all();

	/* init global table and list for at command and general commands */
	prepare_callback();	

	// create and run reading thread
	running = 1;
	ret = pthread_create( &routine_tid, NULL, working_routine, NULL );
	if( ret ) {
		DBG( "start thread error: %s", strerror(ret) );
		goto FAIL;
	}

	DBG( "working_routine started. thread id = %lu", pthread_self() );

	// check nv configs
//	if( clearOpt )
//		reset = true;

	thisDev.reset( new ZigbeeCoord );

	if( clearOpt & STARTOPT_CLEAR_CONFIG ) {
		DBG( "STARTOPT_CLEAR_CONFIG is set" );
		hasReset = true;
		if( ret = write_nv_config(ZCD_NV_STARTUP_OPTION, &clearOpt, 1) )
			goto FAIL;
		zigbee_reset();
		zigbee_start( false );
		clearOpt &= ~STARTOPT_CLEAR_CONFIG;
		do_check_config( type, clearOpt, configChange );
		zigbee_reset();
	} else {
		do_check_config( type, clearOpt, configChange );
	} // if clearOpt

	if( !hasReset )
		zigbee_reset();	// always reset despite configChange
	if( ret = zigbee_start() )
		goto FAIL;
	
	return 0;
FAIL:
	WAIT( "Zigbee init fail, press Enter to go on." );
	DBG( "zigbee init fail, retcode = %d", ret );
	if( routine_tid ) {
		zigbee_finalize();
	} else {
		running = 0;
		routine_tid = 0;
		clear_all();
		close( fd );
	}
	return ret;
}

int zigbee_finalize()
{
	using namespace std;

	// get version cmd
	const uint8 cmdBuf[] = {0xFE, 0x0, 0x21, 0x02, 0x23};
	
	DBG( "zigbee_finalize" );

	running = 0;

	// issue a comand to generate feedback output in order to prevent working routine blocked in read
	if( writen( zigbee_fd, cmdBuf, sizeof(cmdBuf) ) != sizeof(cmdBuf) )
		return -1;
	pthread_join( routine_tid, NULL );
	DBG( "working routine terminated." );
	routine_tid = 0;
	
	close( zigbee_fd );

	clear_all();
	
	return 0;
}





#ifdef _DEBUG

int zigbee_ep_desc( uint16 dstAddr, uint8 ep )
{
	int 								ret;
	ZigbeePayload						cmdData(5);
	std::auto_ptr<ZigbeePayload> 		result;
	const uint8							*ptr;

	DBG( "zigbee_ep_desc() %04x:%02x", dstAddr, ep );
#ifdef HOST_ZDEV_ENDIAN_DIFF
	BYTESWAP( &dstAddr, uint16 );
#endif
	cmdData.appendData( &dstAddr, 2 );
	cmdData.appendData( &dstAddr, 2 );
	cmdData.appendData( ep );

	if( (ret = do_zdev_cmd(ZDO_SIMPLE_DESC_REQ, ZDO_SIMPLE_DESC_REQ_SRSP, cmdData.data(), cmdData.dataLen())) ) {
		DBG( "do_zdev_cmd() error" );
		return ret;
	} // if

	result.reset( (ZigbeePayload*)(cmdSRSPCallbackPtr->getCbData()) );
	ptr = result->data();	
	ret = *ptr;	

	return ret;
}

int zigbee_get_active_ep( uint16 dstAddr )
{
	int 								ret;
	uint16								thisAddr;
	ZigbeePayload						cmdData(4);
	std::auto_ptr<ZigbeePayload> 		result;
	const uint8							*ptr;

	DBG( "zigbee_get_active_ep() %04x", dstAddr );

//	thisAddr = thisDev->getAddr().getNwkAddr();
	thisAddr = dstAddr;

#ifdef HOST_ZDEV_ENDIAN_DIFF
	BYTESWAP( &thisAddr, uint16 );
#endif
	cmdData.appendData( &thisAddr, 2 );
#ifdef HOST_ZDEV_ENDIAN_DIFF
	BYTESWAP( &dstAddr, uint16 );
#endif
	cmdData.appendData( &dstAddr, 2 );

	if( (ret = do_zdev_cmd(ZDO_ACTIVE_EP_REQ, ZDO_ACTIVE_EP_REQ_SRSP, cmdData.data(), cmdData.dataLen())) ) {
		DBG( "do_zdev_cmd() error" );
		return ret;
	} // if

	result.reset( (ZigbeePayload*)(cmdSRSPCallbackPtr->getCbData()) );
	ptr = result->data();	
	ret = *ptr;

	return ret;
}

// testing
void testUtil()
{
	int ret;

	printf( "testUtil() start...\n" );
	printf( "Press Enter to go on...\n" );
	getchar();

//	{
//		uint16 nwkAddr;
//		printf( "test zigbee_get_active_ep():\n" );
//		scanf( "%x", &nwkAddr );
//		getchar();
//		zigbee_get_active_ep( nwkAddr );
//	}

	// read_nv_config
	{
		uint8 buf[20], len;
		if( ret = read_nv_config( ZCD_NV_LOGICAL_TYPE, buf, &len ) ) {
			printf( "read_nv_config test fail %d\n", ret );
			return;
		}
//		DBG( "length = %d\n", len );
		DBG2( "ZCD_NV_LOGICAL_TYPE = " );
		pr_mem( buf, len );

		if( ret = read_nv_config( ZCD_NV_STARTUP_OPTION, buf, &len ) ) {
			printf( "read_nv_config test fail %d\n", ret );
			return;
		}
		DBG2( "ZCD_NV_STARTUP_OPTION = " );
		pr_mem( buf, len );

		if( ret = read_nv_config( ZCD_NV_PANID, buf, &len ) ) {
			printf( "read_nv_config test fail %d\n", ret );
			return;
		}
		DBG2( "ZCD_NV_PANID = " );
		pr_mem( buf, len );

		if( ret = read_nv_config( ZCD_NV_CHANLIST, buf, &len ) ) {
			printf( "read_nv_config test fail %d\n", ret );
			return;
		}
		DBG2( "ZCD_NV_CHANLIST = " );
		pr_mem( buf, len );		

		if( ret = read_nv_config( ZCD_NV_ZDO_DIRECT_CB, buf, &len ) ) {
			printf( "read_nv_config test fail %d\n", ret );
			return;
		}
		DBG2( "ZCD_NV_ZDO_DIRECT_CB = " );
		pr_mem( buf, len );		
	}

	// write_nv_config
//	{
//		uint16 panID = 0xFFFE;
//		int ret = write_nv_config( ZCD_NV_PANID, &panID, 2 );
//	}

	// check_nv_config
//	{
//		bool change = false;
//		uint16 panID = 0xFFFF;
//		int ret = check_nv_config( ZCD_NV_PANID, &panID, 2, change );
//		if( change )
//			DBG( "VALUE CHANGED!" );
//	}

	// reset
//	{
//		zigbee_reset();
//	}

	printf( "testUtil() end\n" );
	return;	
}


#endif	//_DEBUG







