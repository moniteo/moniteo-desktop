#include "SerialPort.h"

#include "Util.h"
#include "serial.h"

#include <iomanip>
#include <memory>
#include <locale.h>
#include <regex>
#include <iostream>


static std::vector<SerialInfos> gSerialList; // System list of com ports


#ifdef USE_WINDOWS_OS

#include <windows.h>
#include <setupapi.h>
#include <devpkey.h>
#include <tchar.h>
#include <initguid.h>
#include <devpropdef.h>


//static CPortsArray ports;

DEFINE_GUID(GUID_DEVCLASS_PORTS, 0x4D36E978, 0xE325, 0x11CE, 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18 );


std::string getDeviceProperty(HDEVINFO devInfo, PSP_DEVINFO_DATA devData, DWORD property)
{
    DWORD buffSize = 0;
    std::string result;
    SetupDiGetDeviceRegistryProperty(devInfo, devData, property, nullptr, nullptr, 0, & buffSize);

    if (buffSize > 0)
    {
        BYTE* buff = new BYTE[buffSize];
        SetupDiGetDeviceRegistryProperty(devInfo, devData, property, nullptr, buff, buffSize, nullptr);
        result = Util::ToString(std::wstring(reinterpret_cast<wchar_t*>(buff)));
        delete [] buff;
    }
    return result;
}


std::string getRegKeyValue(HKEY key, LPCTSTR property)
{
    DWORD size = 0;
    DWORD type;
    RegQueryValueEx(key, property, nullptr, nullptr, nullptr, & size);

    std::string result;

    if (size > 0)
    {
        BYTE* buff = new BYTE[size];
        if( RegQueryValueEx(key, property, nullptr, &type, buff, & size) == ERROR_SUCCESS )
            result = Util::ToString(std::wstring(reinterpret_cast<wchar_t*>(buff)));
        RegCloseKey(key);
        delete [] buff;
    }
    return result;
}

#define		CP210x_SUCCESS						0x00

// Prototype found in CP210x header file
typedef int (WINAPI *CP210x_GetDeviceSerialNumber_t)(
        const HANDLE cyHandle,
        LPVOID	lpSerialNumber,
        LPBYTE	lpbLength,
        const BOOL	bConvertToASCII );

void GetCP210xSerialNumber(SerialInfos &entry)
{
    char full_path[32] = {0};

    HANDLE hCom = nullptr;

    _snprintf(full_path, sizeof(full_path) - 1, "\\\\.\\%s", entry.portName.c_str());

    hCom = CreateFileA(full_path, GENERIC_WRITE | GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);

    if ((hCom != nullptr) && (reinterpret_cast<std::uint64_t>(hCom) != static_cast<std::uint64_t>(-1)))
    {
        HMODULE hModule = LoadLibrary(TEXT("CP210xManufacturing.dll"));

        CP210x_GetDeviceSerialNumber_t getSerial =
                reinterpret_cast<CP210x_GetDeviceSerialNumber_t>(GetProcAddress(hModule, "CP210x_GetDeviceSerialNumber"));

        char	lpSerialNumber[1024];
        BYTE	lpbLength;

        int ret = getSerial(hCom, lpSerialNumber, &lpbLength, TRUE);

        if (ret == CP210x_SUCCESS)
        {
            entry.serial.assign(lpSerialNumber);
            entry.isCypress = true;
        }

        FreeLibrary(hModule);

        CloseHandle(hCom);
    }
}


void GetFTDISerialNumber(SerialInfos &entry)
{
    // FTDIBUS\VID_0403+PID_6001+FTGXU599A\0000

    std::regex pattern(R"(FTDIBUS\\VID_(\d\d\d\d)\+PID_(\d\d\d\d)\+(\w+)\\(.+))");
    std::smatch matcher;
    std::string subMatch;

    std::regex_search(entry.pnpId, matcher, pattern);

    if (matcher.size() >= 4)
    {
        entry.serial = matcher[3].str();
        entry.isFtdi = true;
    }
}
#endif


#ifdef USE_UNIX_OS
#include <stdlib.h>
#include <dirent.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <libgen.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <linux/serial.h>


static std::string get_driver(const std::string& tty)
{
    struct stat st;
    std::string devicedir = tty;

    // Append '/device' to the tty-path
    devicedir += "/device";

    // Stat the devicedir and handle it if it is a symlink
    if (lstat(devicedir.c_str(), &st)==0 && S_ISLNK(st.st_mode))
    {
        char buffer[1024];
        memset(buffer, 0, sizeof(buffer));

        // Append '/driver' and return basename of the target
        devicedir += "/driver";

        if (readlink(devicedir.c_str(), buffer, sizeof(buffer)) > 0)
        {
            return basename(buffer);
        }
    }
    return "";
}

static bool IsRealDevice(const SerialInfos &entry)
{
    bool realDevice = false;
    struct serial_struct serinfo;

    // serial8250-devices must be probe to check for validity
    if (entry.physName == "serial8250")
    {
        // Try to open the device
        int fd = open(entry.portName.c_str(), O_RDWR | O_NONBLOCK | O_NOCTTY);

        if (fd >= 0)
        {
            // Get serial_info
            if (ioctl(fd, TIOCGSERIAL, &serinfo)==0)
            {
                // If device type is no PORT_UNKNOWN we accept the port
                if (serinfo.type != PORT_UNKNOWN)
                {
                    realDevice = true;
                }
            }
            close(fd);
        }
    }
    else
    {
        // Add other case test if we discover other dummy serial devices
        realDevice = true;
    }

    return realDevice;
}

static void RegisterComPort(const std::string& dir)
{
    SerialInfos entry;

    // Get the driver the device is using
    entry.physName = get_driver(dir);

    // Skip devices without a driver
    if (entry.physName.size() > 0)
    {
        std::string file = Util::GetFileName(dir);
        entry.portName = std::string("/dev/") + file;

        if (IsRealDevice(entry))
        {
            gSerialList.push_back(entry);
        }
    }
}


#endif


// Sources of information: NodeJS serialport project, Qt serialport
void SerialPort::EnumeratePorts()
{

#ifdef USE_WINDOWS_OS
    HDEVINFO DeviceInfoSet = SetupDiGetClassDevs(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);

    const auto INVALID_HANDLE = reinterpret_cast<HANDLE>(-1);

    gSerialList.clear();

    if (INVALID_HANDLE != DeviceInfoSet)
    {
        SP_DEVINFO_DATA devInfoData;
        devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
        for(DWORD i = 0; SetupDiEnumDeviceInfo(DeviceInfoSet, i, &devInfoData); i++)
        {
            SerialInfos entry;

            WCHAR szBuffer[1024];
            DWORD dwSize = 1024;
            DWORD realSize = 0;
            SetupDiGetDeviceInstanceId(DeviceInfoSet, &devInfoData, &szBuffer[0], dwSize, &realSize);
            szBuffer[realSize] = '\0';
            entry.pnpId = Util::ToString(std::wstring(szBuffer));

            entry.friendName  = getDeviceProperty(DeviceInfoSet, &devInfoData, SPDRP_FRIENDLYNAME);
            entry.physName = getDeviceProperty(DeviceInfoSet, &devInfoData, SPDRP_PHYSICAL_DEVICE_OBJECT_NAME);
            entry.enumName = getDeviceProperty(DeviceInfoSet, &devInfoData, SPDRP_ENUMERATOR_NAME);
            entry.hardwareIDs = Util::ToUpper(getDeviceProperty(DeviceInfoSet, &devInfoData, SPDRP_HARDWAREID));
            entry.manufacturer = getDeviceProperty(DeviceInfoSet, &devInfoData, SPDRP_MFG);

            HKEY devKey = SetupDiOpenDevRegKey(DeviceInfoSet, &devInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            entry.portName = getRegKeyValue(devKey, TEXT("PortName"));

            std::regex pattern("VID_(\\w+)&PID_(\\w+)");
            std::smatch matcher;
            std::string subMatch;

            std::regex_search(entry.hardwareIDs, matcher, pattern);

            if (matcher.size() >= 2)
            {
                // Extracted value is located at index 1
                entry.vendorId = matcher[1].str();
                entry.productId = matcher[2].str();
            }
#ifdef WITH_CP21DLL
            GetCP210xSerialNumber(entry);
#endif
            GetFTDISerialNumber(entry);

            std::cout << entry.friendName  << "\r\n"
                      << "    " << entry.physName << "\r\n"
                      << "    " << entry.enumName << "\r\n"
                      << "    " << entry.pnpId << "\r\n"
                      << "    " << entry.manufacturer << "\r\n"
                      << "    " << entry.hardwareIDs << " ==> VendorID: " << entry.vendorId << " ProductId: " << entry.productId << "\r\n"
                      << "    " << entry.portName << std::endl;
            if (entry.isFtdi)
            {
                std::cout << "    FTDI chip with serial: " << entry.serial << std::endl;
            }
            if (entry.isCypress)
            {
                std::cout << "    Cypress chip with serial: " << entry.serial << std::endl;
            }

            gSerialList.push_back(entry);
        }
    }

    if (DeviceInfoSet)
    {
        SetupDiDestroyDeviceInfoList(DeviceInfoSet);
    }    

#endif

#ifdef USE_UNIX_OS
    int n;
    struct dirent **namelist;

    const char* sysdir = "/sys/class/tty/";

    // Scan through /sys/class/tty - it contains all tty-devices in the system
    n = scandir(sysdir, &namelist, nullptr, nullptr);
    if (n < 0)
    {
        perror("scandir");
    }
    else
    {
        while (n--)
        {
            if (strcmp(namelist[n]->d_name,"..") && strcmp(namelist[n]->d_name,"."))
            {
                // Construct full absolute file path
                std::string devicedir = sysdir;
                devicedir += namelist[n]->d_name;

                // Register the device
                RegisterComPort(devicedir);
            }
            free(namelist[n]);
        }
        free(namelist);
    }

#endif
}



std::vector<SerialInfos> SerialPort::GetList()
{
    return gSerialList;
}

int32_t SerialPort::AssociatePort(const std::string &ident, std::string &portName)
{
    int32_t retCode = cPortNotFound;
    for (auto &p : gSerialList)
    {
        if ((p.portName == ident) ||
            (p.friendName == ident) ||
            (p.FullUsbId() == ident) ||
            (p.serial == ident))
        {
            if (p.associated == false)
            {
                retCode = cPortAssociated;
                p.associated = true;
                portName = p.portName;
              //  std::cout << "Associated serial device: " << ident << " port name: " << portName << std::endl;
            }
            else
            {
                retCode = cPortNotFree;
            }
            break;
        }
    }

    return retCode;
}


SerialPort::SerialPort()
    : mFd(-1)
{

}

SerialPort::~SerialPort()
{

}

std::int32_t SerialPort::Open(const std::string &ident, const std::string &params)
{
    std::string portName;
    // First, find com port number using the identifier
    std::int32_t retCode = SerialPort::AssociatePort(ident, portName);
    if (retCode == cPortAssociated)
    {
        mFd = serial_open(portName.c_str());
        if (mFd < 0)
        {
            mLastError = "Cannot open serial port: " + portName;
            retCode = cPortOpenError;
        }
        else
        {
            // Eg: 9600,8,N,1
            std::vector<std::string> paramList = Util::Split(params, ",");
            if (paramList.size() == 4)
            {
                try {
                    unsigned long baudrate = static_cast<unsigned long>(std::stol(paramList[0]));
                    serial_setup(mFd, baudrate);
                    mLastSuccess = "Setup serial port " + portName + " success at " + std::to_string(baudrate) + " bauds";
                } catch (const std::exception & e) {
                    mLastError = "Bad device parameters (expected integers): " +  params + ". Error: " + e.what();
                }
            }
            else
            {
                mLastError = "Serial port parameter needs four comma separated parameters, eg: 9600,8,N,1. Got: " + params;
                retCode = cPortBadParameters;
            }
        }
    }
    else
    {
        mLastError = "Cannot find COM port with identifier: " + ident + ", please verify the parameters or device not connected";
    }

    if (retCode != cPortAssociated)
    {
        Close();
    }

    return retCode;
}

void SerialPort::Close()
{
    mFd = serial_close(mFd);
}

int32_t SerialPort::Write(const uint8_t *data, std::uint32_t size)
{
    return Write(std::string(reinterpret_cast<const char*>(data), size));
}

int32_t SerialPort::Write(const std::string &data)
{
    std::int32_t retCode = cPortWriteError;
    if (mFd != -1)
    {
        std::int32_t ret = serial_write(mFd, data.c_str(), static_cast<int>(data.size()));

        if (ret == static_cast<std::int32_t>(data.size()))
        {
            retCode = cPortWriteSuccess;
        }
    }
    else
    {
        retCode = cPortNotOpen;
        mLastError = "Cannot write to serial port, not opened!";
    }

    return retCode;
}

int32_t SerialPort::Read(std::string &data, std::int32_t timeout_sec)
{
    std::int32_t retCode = cPortReadError;
    if (mFd != -1)
    {
        std::int32_t ret = serial_read(mFd, mBuffer, COM_PORT_BUF_SIZE, timeout_sec);

        if (ret > 0)
        {
            data.assign(mBuffer, static_cast<std::uint32_t>(ret));
            retCode = cPortReadSuccess;
        }
        else
        {
            retCode = cPortReadTimeout;
        }
    }
    else
    {
        retCode = cPortNotOpen;
        mLastError = "Cannot read to serial port, not opened!";
    }

    return retCode;


}

bool SerialPort::IsOpen() const
{
    return (mFd < 0) ? false : true;
}

std::string SerialPort::GetLastError()
{
    return mLastError;
}

std::string SerialPort::GetLastSuccess()
{
    return mLastSuccess;
}
