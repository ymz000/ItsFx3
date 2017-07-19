#include "fx3commands.h"

#include "fx3devcyapi.h"
#ifndef NO_CY_API

#include "HexParser.h"
#include "ad9361/ad9361_tuner.h"


FX3DevCyAPI::FX3DevCyAPI() :
    data_handler( NULL ),
    last_overflow_count( 0 ),
    size_tx_mb( 0.0 )
{

}

FX3DevCyAPI::~FX3DevCyAPI() {
    stopRead();

    std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );

    if ( resetFx3Chip() == FX3_ERR_OK ) {
        fprintf( stderr, "Fx3 is going to reset. Please wait\n" );
        for ( int i = 0; i < 20; i++ ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
            fprintf( stderr, "*" );
        }
        fprintf( stderr, " done\n" );
    } else {
        fprintf( stderr, "__error__ FX3 CHIP RESET failed\n" );
    }
}

fx3_dev_err_t FX3DevCyAPI::init(const char *firmwareFileName, const char *additionalFirmwareFileName) {
    StartParams.USBDevice = new CCyFX3Device();
    int boot = 0;
    int stream = 0;
    fx3_dev_err_t res = scan( boot, stream );
    if ( res != FX3_ERR_OK ) {
        return res;
    }

    bool need_fw_load = stream == 0 && boot > 0;

    if ( need_fw_load ) {
        fprintf( stderr, "FX3DevCyAPI::init() fw load needed\n" );

        FILE* f = fopen( firmwareFileName, "r" );
        if ( !f ) {
            return FX3_ERR_FIRMWARE_FILE_IO_ERROR;
        }

        if ( StartParams.USBDevice->IsBootLoaderRunning() ) {
            int retCode  = StartParams.USBDevice->DownloadFw((char*)firmwareFileName, FX3_FWDWNLOAD_MEDIA_TYPE::RAM);

            if ( retCode != FX3_FWDWNLOAD_ERROR_CODE::SUCCESS ) {
                switch( retCode ) {
                    case INVALID_FILE:
                    case CORRUPT_FIRMWARE_IMAGE_FILE:
                    case INCORRECT_IMAGE_LENGTH:
                    case INVALID_FWSIGNATURE:
                        return FX3_ERR_FIRMWARE_FILE_CORRUPTED;
                    default:
                        return FX3_ERR_CTRL_TX_FAIL;
                }
            } else {
                fprintf( stderr, "FX3DevCyAPI::init() boot ok!\n" );
            }
        } else {
            fprintf( stderr, "__error__ FX3DevCyAPI::init() StartParams.USBDevice->IsBootLoaderRunning() is FALSE\n" );
            return FX3_ERR_BAD_DEVICE;
        }
        last_overflow_count = 0;
    }

    if ( need_fw_load ) {
        int PAUSE_AFTER_FLASH_SECONDS = 2;
        fprintf( stderr, "FX3DevCyAPI::Init() flash completed!\nPlease wait for %d seconds\n", PAUSE_AFTER_FLASH_SECONDS );
        for ( int i = 0; i < PAUSE_AFTER_FLASH_SECONDS * 2; i++ ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
            fprintf( stderr, "*" );
        }
        fprintf( stderr, "\n" );

        delete StartParams.USBDevice;
        StartParams.USBDevice = new CCyFX3Device();
        res = scan( boot, stream );
        if ( res != FX3_ERR_OK ) {
            return res;
        }
    }

    res = prepareEndPoints();
    if ( res != FX3_ERR_OK ) {
        return res;
    }

    pre_init_fx3();
    GetNt1065ChipID();

    if ( additionalFirmwareFileName != NULL ) {
        if ( additionalFirmwareFileName[ 0 ] != 0 ) {

            if ( std::string("manual") == std::string(additionalFirmwareFileName) ) {

                init_ntlab_default();

            } else {

                res = loadAdditionalFirmware( additionalFirmwareFileName, 48 );
                if ( res != FX3_ERR_OK ) {
                    fprintf( stderr, "FX3DevCyAPI::Init() __error__ loadAdditionalFirmware %d %s\n", res, fx3_get_error_string( res ) );
                    return res;
                }

            }
        }
    }
    readNtReg(0x07);

    bool In;
    int Attr, MaxPktSize, MaxBurst, Interface, Address;
    int EndPointsCount = EndPointsParams.size();
    for(int i = 0; i < EndPointsCount; i++){
        GetEndPointParamsByInd(i, &Attr, &In, &MaxPktSize, &MaxBurst, &Interface, &Address);
        fprintf( stderr, "FX3DevCyAPI::Init() EndPoint[%d], Attr=%d, In=%d, MaxPktSize=%d, MaxBurst=%d, Infce=%d, Addr=%d\n",
                 i, Attr, In, MaxPktSize, MaxBurst, Interface, Address);
    }

    return res;
}

void FX3DevCyAPI::startRead(DeviceDataHandlerIfce *handler) {
    size_tx_mb = 0.0;
    startTransferData(0, 128, 4, 1500);
    data_handler = handler;
    xfer_thread = std::thread(&FX3DevCyAPI::xfer_loop, this);
    startGpif();
}

void FX3DevCyAPI::stopRead() {
    if( StartParams.bStreaming ) {
        StartParams.bStreaming = false;
        xfer_thread.join();
        data_handler = NULL;
    }
    fprintf( stderr, "FX3DevCyAPI::stopRead() all done!\n" );
}

void FX3DevCyAPI::sendAttCommand5bits(uint32_t bits) {
    UCHAR buf[16];
    buf[0]=(UCHAR)(bits);

    fprintf( stderr, "FX3DevCyAPI::sendAttCommand5bits() %0x02X\n", bits );
     
    long len=16;
    int success=StartParams.USBDevice->BulkOutEndPt->XferData(&buf[0],len);
    
    if ( !success ) {
        fprintf( stderr, "__error__ FX3DevCyAPI::sendAttCommand5bits() BulkOutEndPt->XferData return FALSE\n" );
    }
    
}

fx3_dev_debug_info_t FX3DevCyAPI::getDebugInfoFromBoard(bool ask_speed_only) {
    if ( ask_speed_only ) {
        fx3_dev_debug_info_t info;
        info.status = FX3_ERR_OK;
        info.size_tx_mb_inc = size_tx_mb;
        size_tx_mb = 0.0;
        return info;
    } else {
        UCHAR buf[32];

        CCyControlEndPoint* CtrlEndPt;
        CtrlEndPt = StartParams.USBDevice->ControlEndPt;
        CtrlEndPt->Target    = TGT_DEVICE;
        CtrlEndPt->ReqType   = REQ_VENDOR;
        CtrlEndPt->Direction = DIR_FROM_DEVICE;
        CtrlEndPt->ReqCode = 0xB4;
        CtrlEndPt->Value = 0;
        CtrlEndPt->Index = 1;
        long len=32;
        int success = CtrlEndPt->XferData( &buf[0], len );
        unsigned int* uibuf = ( unsigned int* ) &buf[0];

        fx3_dev_debug_info_t info;
        if ( !success ) {
            fprintf( stderr, "FX3DevCyAPI::getDebugInfoFromBoard() __error__  CtrlEndPt->XferData is FALSE\n" );
            info.status = FX3_ERR_CTRL_TX_FAIL;
        } else {
            info.status = FX3_ERR_OK;
        }

        info.transfers   = uibuf[ 0 ];
        info.overflows   = uibuf[ 1 ];
        info.phy_err_inc = uibuf[ 2 ];
        info.lnk_err_inc = uibuf[ 3 ];
        info.err_reg_hex = uibuf[ 4 ];
        info.phy_errs    = uibuf[ 5 ];
        info.lnk_errs    = uibuf[ 6 ];

        info.size_tx_mb_inc = size_tx_mb;
        size_tx_mb = 0.0;

        info.overflows_inc = info.overflows - last_overflow_count;
        last_overflow_count = info.overflows;
        return info;
    }
}

fx3_dev_err_t FX3DevCyAPI::scan(int &loadable_count , int &streamable_count) {
    streamable_count = 0;
    loadable_count = 0;
    if (StartParams.USBDevice == NULL) {
        fprintf( stderr, "FX3DevCyAPI::scan() USBDevice == NULL" );
        return FX3_ERR_USB_INIT_FAIL;
    }
    unsigned short product = StartParams.USBDevice->ProductID;
    unsigned short vendor  = StartParams.USBDevice->VendorID;
    fprintf( stderr, "Device: 0x%04X 0x%04X ", vendor, product );
    if ( vendor == VENDOR_ID && product == PRODUCT_STREAM ) {
        fprintf( stderr, " STREAM\n" );
        streamable_count++;
    } else if ( vendor == VENDOR_ID && product == PRODUCT_BOOT ) {
        fprintf( stderr,  "BOOT\n" );
        loadable_count++;
    }
    fprintf( stderr, "\n" );

    if (loadable_count + streamable_count == 0) {
        return FX3_ERR_BAD_DEVICE;
    } else {
        return FX3_ERR_OK;
    }
}

fx3_dev_err_t FX3DevCyAPI::prepareEndPoints() {

    if ( ( StartParams.USBDevice->VendorID != VENDOR_ID) ||
         ( StartParams.USBDevice->ProductID != PRODUCT_STREAM ) )
    {
        return FX3_ERR_BAD_DEVICE;
    }

    int interfaces = StartParams.USBDevice->AltIntfcCount()+1;

    StartParams.bHighSpeedDevice = StartParams.USBDevice->bHighSpeed;
    StartParams.bSuperSpeedDevice = StartParams.USBDevice->bSuperSpeed;

    for (int i=0; i< interfaces; i++)
    {
        StartParams.USBDevice->SetAltIntfc(i);

        int eptCnt = StartParams.USBDevice->EndPointCount();

        // Fill the EndPointsBox
        for (int e=1; e<eptCnt; e++)
        {
            CCyUSBEndPoint *ept = StartParams.USBDevice->EndPoints[e];
            // INTR, BULK and ISO endpoints are supported.
            if ((ept->Attributes >= 1) && (ept->Attributes <= 3))
            {
                EndPointParams Params;
                Params.Attr = ept->Attributes;
                Params.In = ept->bIn;
                Params.MaxPktSize = ept->MaxPktSize;
                Params.MaxBurst = StartParams.USBDevice->BcdUSB == 0x0300 ? ept->ssmaxburst : 0;
                Params.Interface = i;
                Params.Address = ept->Address;

                EndPointsParams.push_back(Params);
            }
        }
    }

    return FX3_ERR_OK;
}

void FX3DevCyAPI::startTransferData(unsigned int EndPointInd, int PPX, int QueueSize, int TimeOut) {
    if(EndPointInd >= EndPointsParams.size())
        return;
    StartParams.CurEndPoint = EndPointsParams[EndPointInd];
    StartParams.PPX = PPX;
    StartParams.QueueSize = QueueSize;
    StartParams.TimeOut = TimeOut;
    StartParams.bStreaming = true;
    StartParams.ThreadAlreadyStopped = false;
    //StartParams.DataProc = DataProc;

    int alt = StartParams.CurEndPoint.Interface;
    int eptAddr = StartParams.CurEndPoint.Address;
    int clrAlt = (StartParams.USBDevice->AltIntfc() == 0) ? 1 : 0;
    if (! StartParams.USBDevice->SetAltIntfc(alt))
    {
        StartParams.USBDevice->SetAltIntfc(clrAlt); // Cleans-up
        return;
    }

    StartParams.EndPt = StartParams.USBDevice->EndPointOf((UCHAR)eptAddr);
}

void FX3DevCyAPI::AbortXferLoop(StartDataTransferParams *Params, int pending, PUCHAR *buffers, CCyIsoPktInfo **isoPktInfos, PUCHAR *contexts, OVERLAPPED *inOvLap)
{
    //EndPt->Abort(); - This is disabled to make sure that while application is doing IO and user unplug the device, this function hang the app.
    long len = Params->EndPt->MaxPktSize * Params->PPX;

    for (int j=0; j< Params->QueueSize; j++)
    {
        if (j<pending)
        {
            if (!Params->EndPt->WaitForXfer(&inOvLap[j], Params->TimeOut))
            {
                Params->EndPt->Abort();
                if (Params->EndPt->LastError == ERROR_IO_PENDING)
                    WaitForSingleObject(inOvLap[j].hEvent,2000);
            }

            Params->EndPt->FinishDataXfer(buffers[j], len, &inOvLap[j], contexts[j]);
        }

        CloseHandle(inOvLap[j].hEvent);

        delete [] buffers[j];
        delete [] isoPktInfos[j];
    }

    delete [] buffers;
    delete [] isoPktInfos;
    delete [] contexts;

    Params->bStreaming = false;
    Params->ThreadAlreadyStopped = true;
}

void FX3DevCyAPI::GetEndPointParamsByInd(unsigned int EndPointInd, int *Attr, bool *In, int *MaxPktSize, int *MaxBurst, int *Interface, int *Address) {
    if(EndPointInd >= EndPointsParams.size())
        return;
    *Attr = EndPointsParams[EndPointInd].Attr;
    *In = EndPointsParams[EndPointInd].In;
    *MaxPktSize = EndPointsParams[EndPointInd].MaxPktSize;
    *MaxBurst = EndPointsParams[EndPointInd].MaxBurst;
    *Interface = EndPointsParams[EndPointInd].Interface;
    *Address = EndPointsParams[EndPointInd].Address;
}

fx3_dev_err_t FX3DevCyAPI::loadAdditionalFirmware(const char* fw_name, uint32_t stop_addr) {
    if ( !fw_name ) {
        return FX3_ERR_ADDFIRMWARE_FILE_IO_ERROR;
    }
    
    FILE* f = fopen( fw_name, "r" );
    if ( !f ) {
        fprintf( stderr, "FX3Dev::loadAdditionalFirmware __error__ can't open '%s'\n", fw_name );
        return FX3_ERR_ADDFIRMWARE_FILE_IO_ERROR;
    } else {
        fclose( f );
    }
    
    std::vector<uint32_t> addr;
    std::vector<uint32_t> data;
    
    parse_hex_file( fw_name, addr, data );
    
    for ( uint32_t i = 0; i < addr.size(); i++ ) {
        fx3_dev_err_t eres = send16bitSPI(data[i], addr[i]);
        if ( eres != FX3_ERR_OK ) {
            return eres;
        }
        
        std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );

        if ( addr[i] == stop_addr ) {
            break;
        }
    }
    return FX3_ERR_OK;
}

fx3_dev_err_t FX3DevCyAPI::send16bitSPI(unsigned char data, unsigned char addr) {
    UCHAR buf[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    buf[0] = (UCHAR)(data);
    buf[1] = (UCHAR)(addr);
    
    //fprintf( stderr, "FX3Dev::send16bitToDevice( 0x%02X, 0x%02X )\n", data, addr );
    
    CCyControlEndPoint* CtrlEndPt;
    CtrlEndPt = StartParams.USBDevice->ControlEndPt;
    CtrlEndPt->Target = TGT_DEVICE;
    CtrlEndPt->ReqType = REQ_VENDOR;
    CtrlEndPt->Direction = DIR_TO_DEVICE;
    CtrlEndPt->ReqCode = fx3cmd::REG_WRITE;
    CtrlEndPt->Value = 0;
    CtrlEndPt->Index = 1;
    long len = 16;
    int success = CtrlEndPt->XferData(&buf[0], len);
    
    return success ? FX3_ERR_OK : FX3_ERR_CTRL_TX_FAIL;
}


fx3_dev_err_t FX3DevCyAPI::read16bitSPI(unsigned char addr, unsigned char* data)
{
    UCHAR buf[16];
    //addr |= 0x8000;
    buf[0] = (UCHAR)(*data);
    buf[1] = (UCHAR)(addr|0x80);

    fprintf( stderr, "FX3Dev::read16bitSPI() from  0x%02X\n", addr );

    CCyControlEndPoint* CtrlEndPt;
    CtrlEndPt = StartParams.USBDevice->ControlEndPt;
    CtrlEndPt->Target = TGT_DEVICE;
    CtrlEndPt->ReqType = REQ_VENDOR;
    CtrlEndPt->Direction = DIR_FROM_DEVICE;
    CtrlEndPt->ReqCode = fx3cmd::REG_READ;
    CtrlEndPt->Value = *data;
    CtrlEndPt->Index = addr|0x80;
    long len = 16;
    int success = CtrlEndPt->XferData(&buf[0], len);

    *data = buf[0];
    if ( success ) {
        fprintf( stderr, "[0x%02X] is 0x%02X\n", addr, *data );
    } else {
        fprintf( stderr, "__error__ FX3Dev::read16bitSPI() FAILED\n" );
    }

    return success ? FX3_ERR_OK : FX3_ERR_CTRL_TX_FAIL;
}




fx3_dev_err_t FX3DevCyAPI::send24bitSPI(unsigned char data, unsigned short addr)
{
    fprintf( stderr, "[0x%03X] <= 0x%02X\n", addr, data );

    UCHAR buf[16];
    addr |= 0x8000;
    buf[0] = (UCHAR)(data);
    buf[1] = (UCHAR)(addr&0x0FF);
    buf[2] = (UCHAR)(addr>>8);

    CCyControlEndPoint* CtrlEndPt;
    CtrlEndPt = StartParams.USBDevice->ControlEndPt;
    CtrlEndPt->Target = TGT_DEVICE;
    CtrlEndPt->ReqType = REQ_VENDOR;
    CtrlEndPt->Direction = DIR_TO_DEVICE;
    CtrlEndPt->ReqCode = 0xB6;
    CtrlEndPt->Value = 0;
    CtrlEndPt->Index = 1;
    long len = 16;
    int success = CtrlEndPt->XferData(&buf[0], len);

    if ( !success ) {
        fprintf( stderr, "__error__ FX3Dev::send24bitSPI() FAILED (len = %d)\n", len );
    }

    return success ? FX3_ERR_OK : FX3_ERR_CTRL_TX_FAIL;;
}

fx3_dev_err_t FX3DevCyAPI::read24bitSPI(unsigned short addr, unsigned char* data)
{
    UCHAR buf[16];
    //addr |= 0x8000;
    buf[0] = (UCHAR)(*data);
    buf[1] = (UCHAR)(addr&0x0FF);
    buf[2] = (UCHAR)(addr>>8);

    fprintf( stderr, "FX3Dev::read24bitSPI() from  0x%03X\n", addr );

    CCyControlEndPoint* CtrlEndPt;
    CtrlEndPt = StartParams.USBDevice->ControlEndPt;
    CtrlEndPt->Target = TGT_DEVICE;
    CtrlEndPt->ReqType = REQ_VENDOR;
    CtrlEndPt->Direction = DIR_FROM_DEVICE;
    CtrlEndPt->ReqCode = 0xB5;
    CtrlEndPt->Value = *data;
    CtrlEndPt->Index = addr;
    long len = 16;
    int success = CtrlEndPt->XferData(&buf[0], len);

    *data = buf[0];
    if ( success ) {
        fprintf( stderr, "[0x%03X] => 0x%02X\n", addr, *data );
    } else {
        fprintf( stderr, "__error__ FX3Dev::read24bitSPI() FAILED\n" );
    }
    return success ? FX3_ERR_OK : FX3_ERR_CTRL_TX_FAIL;
}

fx3_dev_err_t FX3DevCyAPI::resetFx3Chip()
{
    UCHAR buf[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

    fprintf( stderr, "FX3DevCyAPI::resetFx3Chip( )\n" );

    CCyControlEndPoint* CtrlEndPt;
    CtrlEndPt = StartParams.USBDevice->ControlEndPt;
    CtrlEndPt->Target = TGT_DEVICE;
    CtrlEndPt->ReqType = REQ_VENDOR;
    CtrlEndPt->Direction = DIR_TO_DEVICE;
    CtrlEndPt->ReqCode = fx3cmd::CYPRESS_RESET;
    CtrlEndPt->Value = 0;
    CtrlEndPt->Index = 0;
    long len = 16;
    int success = CtrlEndPt->XferData(&buf[0], len);

    return success ? FX3_ERR_OK : FX3_ERR_CTRL_TX_FAIL;
}

void FX3DevCyAPI::pre_init_fx3()
{
    writeGPIO(NT1065EN,  0);
    writeGPIO(NT1065AOK, 0);
    writeGPIO(VCTCXOEN,  0);
    writeGPIO(ANTLNAEN,  0);
    writeGPIO(ANTFEEDEN, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    writeGPIO(VCTCXOEN,  1);
    writeGPIO(NT1065EN,  1);

    writeGPIO(ANTLNAEN,  1);
    writeGPIO(ANTFEEDEN, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

void FX3DevCyAPI::init_ntlab_default()
{
    unsigned char reg = 0;

    //  SetSingleLO
    send16bitSPI(0x00, 3);
    send16bitSPI(0x00, 45);

    // SetPLLTune
    int npll = 0;
    read16bitSPI(43+npll*4, &reg);
    send16bitSPI(reg|1, 43+npll*4);


    //PLLstat = NT1065_GetPLLStat(0);

    //AOK
    readNtReg(0x07);

    // NT1065_SetSideBand( chan, side )
    int chan = 1;
    int side = 1;
    read16bitSPI(13+chan*7, &reg);
    send16bitSPI((reg&0xFD)|((side&1)<<1), 13+chan*7);

    chan = 2;
    side = 1;
    read16bitSPI(13+chan*7, &reg);
    send16bitSPI((reg&0xFD)|((side&1)<<1), 13+chan*7);

    chan = 3;
    side = 1;
    read16bitSPI(13+chan*7, &reg);
    send16bitSPI((reg&0xFD)|((side&1)<<1), 13+chan*7);

    //AOK
    readNtReg(0x07);

    // SetOutAnalog_IFMGC
    send16bitSPI(0x22, 15);
    send16bitSPI(0x22, 22);
    send16bitSPI(0x22, 29);
    send16bitSPI(0x22, 36);

//    // setRFGain
//    chan = 0;
//    int gain = 11;
//    read16bitSPI(17+chan*7, &reg);
//    send16bitSPI((reg&0x3)|(((gain-11)&0x0F)<<4), 17+chan*7);

//    chan = 1;
//    gain = 11;
//    read16bitSPI(17+chan*7, &reg);
//    send16bitSPI((reg&0x3)|(((gain-11)&0x0F)<<4), 17+chan*7);

//    chan = 2;
//    gain = 11;
//    read16bitSPI(17+chan*7, &reg);
//    send16bitSPI((reg&0x3)|(((gain-11)&0x0F)<<4), 17+chan*7);

//    chan = 3;
//    gain = 11;
//    read16bitSPI(17+chan*7, &reg);
//    send16bitSPI((reg&0x3)|(((gain-11)&0x0F)<<4), 17+chan*7);

//    // setIfGain
//    chan = 0;
//    int gain_code = 10;
//    read16bitSPI(17+chan*7, &reg);
//    send16bitSPI((reg&0xFC)|(((gain_code>>3)&0x03)), 17+chan*7);
//    send16bitSPI( (gain_code&0x07)<<5, 18+chan*7);

//    chan = 1;
//    gain_code = 10;
//    read16bitSPI(17+chan*7, &reg);
//    send16bitSPI((reg&0xFC)|(((gain_code>>3)&0x03)), 17+chan*7);
//    send16bitSPI( (gain_code&0x07)<<5, 18+chan*7);

//    chan = 2;
//    gain_code = 10;
//    read16bitSPI(17+chan*7, &reg);
//    send16bitSPI((reg&0xFC)|(((gain_code>>3)&0x03)), 17+chan*7);
//    send16bitSPI( (gain_code&0x07)<<5, 18+chan*7);

//    chan = 3;
//    gain_code = 10;
//    read16bitSPI(17+chan*7, &reg);
//    send16bitSPI((reg&0xFC)|(((gain_code>>3)&0x03)), 17+chan*7);
//    send16bitSPI( (gain_code&0x07)<<5, 18+chan*7);


    // SetLPFCalStart
    send16bitSPI(1, 4);

    // SetOutADC_IFMGC
    send16bitSPI(0x23, 15);
    send16bitSPI(0x0B, 19);
    send16bitSPI(0x23, 22);
    send16bitSPI(0x0B, 26);
    send16bitSPI(0x23, 29);
    send16bitSPI(0x0B, 33);
    send16bitSPI(0x23, 36);
    send16bitSPI(0x0B, 40);
}

uint32_t FX3DevCyAPI::GetNt1065ChipID()
{
    unsigned char reg0 = 0;
    read16bitSPI(0x00, &reg0);

    unsigned char reg1 = 0;
    read16bitSPI(0x01, &reg1);

    uint32_t id = (unsigned int)reg0<<21 | ((unsigned int)reg1&0xF8)<<13 | reg1&0x07;

    fprintf( stderr, "ChipID: %08X\n", id );
    return id;
}

void print_bits( uint32_t val, int bits_count = 8 ) {
    for ( int i = bits_count - 1; i >=0; i-- ) {
        if ( val & ( 1 << i ) ) {
            fprintf( stderr, "  1" );
        } else {
            fprintf( stderr, "  0" );
        }
    }
    fprintf( stderr, "\n" );

    for ( int i = bits_count - 1; i >=0; i-- ) {
        fprintf( stderr, "%3d", i );
    }
    fprintf( stderr, "\n" );
}

void FX3DevCyAPI::readNtReg(uint32_t reg)
{
    fx3_dev_err_t res = FX3_ERR_OK;
    unsigned char val = 0x00;
    res = read16bitSPI(reg, &val);
    fprintf( stderr, "Reg%d (0x%02X), val = 0x%08X\n", reg, reg, val );
    print_bits(val);
}

void FX3DevCyAPI::writeGPIO(uint32_t gpio, uint32_t value)
{
    UCHAR buf[16];

    fprintf( stderr, "writeGPIO( %d, %d )\n", gpio, value );

    CCyControlEndPoint* CtrlEndPt;
    CtrlEndPt = StartParams.USBDevice->ControlEndPt;
    CtrlEndPt->Target = TGT_DEVICE;
    CtrlEndPt->ReqType = REQ_VENDOR;
    CtrlEndPt->Direction = DIR_FROM_DEVICE;
    CtrlEndPt->ReqCode = fx3cmd::WRITE_GPIO;
    CtrlEndPt->Value = value;
    CtrlEndPt->Index = gpio;
    long len = 16;
    int success = CtrlEndPt->XferData(&buf[0], len);

    if ( success ) {
        uint32_t* ans = (uint32_t*)buf;
        if ( ans[0] != 0 ) {
            fprintf(stderr, "writeGPIO( %d, %d ) ERROR CyU3PGpioSetValue returned 0x%02X\n", gpio, value, ans[0] );
        }
    } else {
        fprintf(stderr, "writeGPIO( %d, %d ) FAILED\n", gpio, value );
    }
}

void FX3DevCyAPI::readGPIO(uint32_t gpio, uint32_t *value)
{
    UCHAR buf[16];

    fprintf( stderr, "readGPIO( %d )\n", gpio );

    CCyControlEndPoint* CtrlEndPt;
    CtrlEndPt = StartParams.USBDevice->ControlEndPt;
    CtrlEndPt->Target = TGT_DEVICE;
    CtrlEndPt->ReqType = REQ_VENDOR;
    CtrlEndPt->Direction = DIR_FROM_DEVICE;
    CtrlEndPt->ReqCode = fx3cmd::WRITE_GPIO;
    CtrlEndPt->Value = 0;
    CtrlEndPt->Index = gpio;
    long len = 16;
    int success = CtrlEndPt->XferData(&buf[0], len);

    if ( success ) {
        uint32_t* ans = (uint32_t*)buf;
        if ( ans[0] != 0 ) {
            fprintf(stderr, "readGPIO( %d ) ERROR CyU3PGpioGetValue returned 0x%02X\n", gpio, ans[0] );
        } else {
            fprintf(stderr, "readGPIO( %d ) have %d value \n", gpio, ans[1] );
            *value = ans[1];
        }
    } else {
        fprintf(stderr, "readGPIO( %d ) FAILED\n", gpio );
    }
}

void FX3DevCyAPI::startGpif()
{
    UCHAR buf[16];

    fprintf( stderr, "startGpif()\n" );

    CCyControlEndPoint* CtrlEndPt;
    CtrlEndPt = StartParams.USBDevice->ControlEndPt;
    CtrlEndPt->Target = TGT_DEVICE;
    CtrlEndPt->ReqType = REQ_VENDOR;
    CtrlEndPt->Direction = DIR_FROM_DEVICE;
    CtrlEndPt->ReqCode = fx3cmd::START;
    CtrlEndPt->Value = 0;
    CtrlEndPt->Index = 0;
    long len = 16;
    int success = CtrlEndPt->XferData(&buf[0], len);

    if ( !success ) {
        fprintf(stderr, "startGpif() FAILED\n" );
    }
}


void FX3DevCyAPI::xfer_loop() {
    fprintf( stderr, "FX3DevCyAPI::xfer_loop() started\n" );
    StartDataTransferParams* Params = &StartParams;
    
    
    if(Params->EndPt->MaxPktSize==0)
        return;
    
    // Limit total transfer length to 4MByte
    long len = ((Params->EndPt->MaxPktSize) * Params->PPX);
    
    int maxLen = 0x400000;  //4MByte
    if (len > maxLen){
        Params->PPX = maxLen / (Params->EndPt->MaxPktSize);
        if((Params->PPX%8)!=0)
            Params->PPX -= (Params->PPX%8);
    }
    
    if ((Params->bSuperSpeedDevice || Params->bHighSpeedDevice) && (Params->EndPt->Attributes == 1)){  // HS/SS ISOC Xfers must use PPX >= 8
        Params->PPX = max(Params->PPX, 8);
        Params->PPX = (Params->PPX / 8) * 8;
        if(Params->bHighSpeedDevice)
            Params->PPX = max(Params->PPX, 128);
    }

    long BytesXferred = 0;
    unsigned long Successes = 0;
    unsigned long Failures = 0;
    int i = 0;

    // Allocate the arrays needed for queueing
    PUCHAR			*buffers		= new PUCHAR[Params->QueueSize];
    CCyIsoPktInfo	**isoPktInfos	= new CCyIsoPktInfo*[Params->QueueSize];
    PUCHAR			*contexts		= new PUCHAR[Params->QueueSize];
    OVERLAPPED		inOvLap[MAX_QUEUE_SZ];

    Params->EndPt->SetXferSize(len);

    // Allocate all the buffers for the queues
    for (i=0; i< Params->QueueSize; i++)
    {
        buffers[i]        = new UCHAR[len];
        isoPktInfos[i]    = new CCyIsoPktInfo[Params->PPX];
        inOvLap[i].hEvent = CreateEvent(NULL, false, false, NULL);

        memset(buffers[i],0xEF,len);
    }

    // Queue-up the first batch of transfer requests
    for (i=0; i< Params->QueueSize; i++)
    {
        contexts[i] = Params->EndPt->BeginDataXfer(buffers[i], len, &inOvLap[i]);
        if (Params->EndPt->NtStatus || Params->EndPt->UsbdStatus) // BeginDataXfer failed
        {
            AbortXferLoop(Params, i+1, buffers,isoPktInfos,contexts,inOvLap);
            return;
        }
    }

    i=0;



    // The infinite xfer loop.
    for (;Params->bStreaming;)
    {
        long rLen = len;	// Reset this each time through because
        // FinishDataXfer may modify it

        if (!Params->EndPt->WaitForXfer(&inOvLap[i], Params->TimeOut))
        {
            Params->EndPt->Abort();
            if (Params->EndPt->LastError == ERROR_IO_PENDING)
                WaitForSingleObject(inOvLap[i].hEvent,2000);
        }

        if (Params->EndPt->Attributes == 1) // ISOC Endpoint
        {
            if (Params->EndPt->FinishDataXfer(buffers[i], rLen, &inOvLap[i], contexts[i], isoPktInfos[i]))
            {
                CCyIsoPktInfo *pkts = isoPktInfos[i];
                for (int j=0; j< Params->PPX; j++)
                {
                    if ((pkts[j].Status == 0) && (pkts[j].Length<=Params->EndPt->MaxPktSize))
                    {
                        BytesXferred += pkts[j].Length;
                        Successes++;
                    }
                    else
                        Failures++;
                    pkts[j].Length = 0;	// Reset to zero for re-use.
                    pkts[j].Status = 0;
                }
            }
            else
                Failures++;
        }
        else // BULK Endpoint
        {
            if (Params->EndPt->FinishDataXfer(buffers[i], rLen, &inOvLap[i], contexts[i]))
            {
                Successes++;
                BytesXferred += len;
            }
            else
                Failures++;
        }

        BytesXferred = max(BytesXferred, 0);

        size_tx_mb += ( ( double ) len ) / ( 1024.0 * 1024.0 );
        if ( data_handler ) {
            data_handler->HandleDeviceData(buffers[i], len);
        }

        // Re-submit this queue element to keep the queue full
        contexts[i] = Params->EndPt->BeginDataXfer(buffers[i], len, &inOvLap[i]);
        if (Params->EndPt->NtStatus || Params->EndPt->UsbdStatus) // BeginDataXfer failed
        {
            AbortXferLoop(Params, Params->QueueSize,buffers,isoPktInfos,contexts,inOvLap);
            return;
        }

        i = (i + 1) % Params->QueueSize;
    }  // End of the infinite loop

    AbortXferLoop( Params, Params->QueueSize, buffers, isoPktInfos, contexts, inOvLap );

    fprintf( stderr, "FX3DevCyAPI::xfer_loop() finished\n" );
}

uint8_t FX3DevCyAPI::peek8(uint32_t register_address24) {
    uint8_t val = 0;
    if ( read24bitSPI( register_address24, &val ) == FX3_ERR_OK ) {
        return val;
    } else {
        return 0xFF;
    }
}

void FX3DevCyAPI::poke8(uint32_t register_address24, uint8_t value) {
    send24bitSPI( value, register_address24 );
}

#endif

