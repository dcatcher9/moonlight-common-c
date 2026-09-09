#include "Limelight-internal.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); abort(); } } while (0)

static int receiveResponse(SOCKET socket, char* bytes, int size, int flags);
#define recv receiveResponse
#include "../src/RtspConnection.c"
#undef recv

// Exercise the entire production RTSP handshake and SDP writer with scripted
// TCP responses, including older per-stream PLAY and optional feature failures.
int AppVersionQuad[4];
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
AUDIO_RENDERER_CALLBACKS AudioCallbacks;
struct sockaddr_storage RemoteAddr;
struct sockaddr_storage LocalAddr;
SOCKADDR_LEN AddrLen;
uint16_t RtspPortNumber = 48010;
uint16_t ControlPortNumber;
uint16_t AudioPortNumber;
uint16_t VideoPortNumber;
uint16_t MicPortNumber;
SS_PING AudioPingPayload;
SS_PING VideoPingPayload;
SS_PING MicPingPayload;
uint32_t ControlConnectData;
uint32_t EncryptionFeaturesSupported;
uint32_t EncryptionFeaturesRequested;
uint32_t EncryptionFeaturesEnabled;
uint32_t SunshineFeatureFlags;
bool AudioEncryptionEnabled;
bool HighQualitySurroundSupported;
bool HighQualitySurroundEnabled;
OPUS_MULTISTREAM_CONFIGURATION NormalQualityOpusConfig;
OPUS_MULTISTREAM_CONFIGURATION HighQualityOpusConfig;
int AudioPacketDuration;
bool ReferenceFrameInvalidationSupported;
int NegotiatedVideoFormat;
volatile bool ConnectionInterrupted;

static char responseBytes[4096];
static int responseOffset;
static int responseLength;
static int openSockets;
static int microphoneSetups;
static int microphonePlays;
static int microphoneSetupStatus;
static int microphonePlayStatus;
static const char* microphoneTransport;
static const char* microphonePing;
static bool failMicrophoneSetup;
static bool cancelMicrophoneSetup;
static bool failMicrophonePlay;
static unsigned int hostEncryptionSupport;
static unsigned int hostEncryptionRequest;
static unsigned int announcedEncryption;
static unsigned int announcedClientFeatures;

SOCKET connectTcpSocket(struct sockaddr_storage* address, SOCKADDR_LEN length, unsigned short port, int timeout) {
    CHECK(address == &RemoteAddr && length == AddrLen && port == 48010 && timeout > 0);
    CHECK(openSockets == 0);
    openSockets++;
    return (SOCKET)123;
}
void closeSocket(SOCKET socket) { CHECK(socket == (SOCKET)123 && openSockets == 1); openSockets--; }
int enableNoDelay(SOCKET socket) { CHECK(socket == (SOCKET)123); return 0; }
int pollSockets(struct pollfd* fds, int count, int timeout) {
    CHECK(count == 1 && fds[0].fd == (SOCKET)123 && timeout > 0);
    return 1;
}
void* extendBuffer(void* buffer, size_t size) { return realloc(buffer, size); }
void PltSleepMs(int ms) { (void)ms; CHECK(false); }
uint64_t PltGetMillis(void) { return 100; }
uint64_t LiGetMillis(void) { return 100; }
bool PltSafeStrcpy(char* dest, size_t size, const char* src) {
    if (strlen(src) >= size) return false;
    strcpy(dest, src);
    return true;
}
void addrToUrlSafeString(struct sockaddr_storage* address, char* buffer, size_t size) {
    (void)address;
    CHECK(PltSafeStrcpy(buffer, size, "127.0.0.1"));
}
int notifyAudioPortNegotiationComplete(void) { CHECK(AudioPortNumber != 0); return 0; }
bool isReferenceFrameInvalidationSupportedByDecoder(void) { return false; }
int serviceEnetHost(ENetHost* host, ENetEvent* event, enet_uint32 timeout) {
    (void)host; (void)event; (void)timeout; CHECK(false); return -1;
}

int sendMtuSafe(SOCKET socket, char* bytes, int size) {
    CHECK(socket == (SOCKET)123 && size > 0);
    char headers[1024] = "Session: test-session\r\nTransport: unicast;server_port=47998-47999\r\n";
    char payload[512] = "";
    int status = 200;
    if (strncmp(bytes, "DESCRIBE ", 9) == 0) {
        snprintf(payload, sizeof(payload), "v=0\r\ns=Test\r\na=x-ss-general.encryptionSupported:%u\r\na=x-ss-general.encryptionRequested:%u\r\n",
                 hostEncryptionSupport, hostEncryptionRequest);
    }
    else if (strncmp(bytes, "SETUP streamid=mic", 18) == 0) {
        const char* target = AppVersionQuad[0] >= 5 ? "SETUP streamid=mic/0/0 RTSP/1.0" : "SETUP streamid=mic RTSP/1.0";
        CHECK(strncmp(bytes, target, strlen(target)) == 0);
        microphoneSetups++;
        if (cancelMicrophoneSetup) ConnectionInterrupted = true;
        if (failMicrophoneSetup || cancelMicrophoneSetup) {
            SetLastSocketError(ECONNREFUSED);
            return SOCKET_ERROR;
        }
        status = microphoneSetupStatus;
        snprintf(headers, sizeof(headers), "Session: test-session\r\n%s%s%s%s%s%s",
                 microphoneTransport ? "Transport: " : "", microphoneTransport ? microphoneTransport : "",
                 microphoneTransport ? "\r\n" : "", microphonePing ? "X-SS-Ping-Payload: " : "",
                 microphonePing ? microphonePing : "", microphonePing ? "\r\n" : "");
    }
    else if (strncmp(bytes, "PLAY streamid=mic ", 18) == 0) {
        microphonePlays++;
        if (failMicrophonePlay) { SetLastSocketError(ECONNREFUSED); return SOCKET_ERROR; }
        status = microphonePlayStatus;
    }
    else if (strncmp(bytes, "ANNOUNCE ", 9) == 0) {
        char* encryption = strstr(bytes, "a=x-ss-general.encryptionEnabled:");
        char* features = strstr(bytes, "a=x-ml-general.featureFlags:");
        CHECK(encryption != NULL && features != NULL);
        announcedEncryption = (unsigned int)strtoul(encryption + strlen("a=x-ss-general.encryptionEnabled:"), NULL, 10);
        announcedClientFeatures = (unsigned int)strtoul(features + strlen("a=x-ml-general.featureFlags:"), NULL, 10);
    }
    responseLength = snprintf(responseBytes, sizeof(responseBytes),
        "RTSP/1.0 %d %s\r\nCSeq: 1\r\n%sContent-length: %u\r\n\r\n%s",
        status, status == 200 ? "OK" : "Unsupported", headers, (unsigned int)strlen(payload), payload);
    CHECK(responseLength > 0 && responseLength < (int)sizeof(responseBytes));
    responseOffset = 0;
    return size;
}

static int receiveResponse(SOCKET socket, char* bytes, int size, int flags) {
    CHECK(socket == (SOCKET)123 && flags == 0);
    int available = responseLength - responseOffset;
    if (available > size) available = size;
    memcpy(bytes, responseBytes + responseOffset, available);
    responseOffset += available;
    return available;
}

static void resetHandshake(bool modern, bool redirectMic) {
    CHECK(openSockets == 0);
    memset(&StreamConfig, 0, sizeof(StreamConfig));
    memset(&RemoteAddr, 0, sizeof(RemoteAddr));
    memset(&LocalAddr, 0, sizeof(LocalAddr));
    RemoteAddr.ss_family = LocalAddr.ss_family = AF_INET;
    AddrLen = sizeof(struct sockaddr_in);
    AppVersionQuad[0] = modern ? 7 : 4;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = modern ? 500 : 0;
    AppVersionQuad[3] = -1;
    StreamConfig.width = 1920;
    StreamConfig.height = 1080;
    StreamConfig.fps = 60;
    StreamConfig.packetSize = 1024;
    StreamConfig.bitrate = 20000;
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    StreamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;
    StreamConfig.redirectMic = redirectMic;
    StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
    AudioPortNumber = VideoPortNumber = ControlPortNumber = 0;
    // The handshake itself must clear microphone state from the previous session.
    MicPortNumber = 55555;
    memset(&MicPingPayload, 0x55, sizeof(MicPingPayload));
    ConnectionInterrupted = false;
    microphoneSetupStatus = microphonePlayStatus = 200;
    microphoneTransport = "unicast;server_port=48001-48002";
    microphonePing = "123456789abcdef0";
    microphoneSetups = microphonePlays = 0;
    failMicrophoneSetup = cancelMicrophoneSetup = failMicrophonePlay = false;
    hostEncryptionSupport = hostEncryptionRequest = announcedEncryption = announcedClientFeatures = 0;
}

static int handshake(void) {
    SERVER_INFORMATION server = {0};
    server.serverCodecModeSupport = SCM_H264;
    int result = performRtspHandshake(&server);
    CHECK(openSockets == 0 && sessionIdString == NULL && encryptionCtx == NULL && decryptionCtx == NULL);
    return result;
}

static void testNegotiation(void) {
    for (int modern = 0; modern < 2; modern++) {
        resetHandshake(modern != 0, false);
        CHECK(handshake() == 0 && MicPortNumber == 0 && microphoneSetups == 0 && microphonePlays == 0);
        CHECK(MicPingPayload.payload[0] == 0);
        resetHandshake(modern != 0, true);
        CHECK(handshake() == 0 && MicPortNumber == 48001 && microphoneSetups == 1);
        CHECK(microphonePlays == (modern ? 0 : 1));
        CHECK(memcmp(MicPingPayload.payload, "123456789abcdef0", 16) == 0);
        CHECK(MicPingPayload.sequenceNumber == 0);
        CHECK((announcedClientFeatures & (ML_FF_HOST_SBS_TELEMETRY_V2 | ML_FF_ATOMIC_PRESENTATION_MODE_V2 | ML_FF_SOURCE_FRAME_ID_V1)) ==
              (ML_FF_HOST_SBS_TELEMETRY_V2 | ML_FF_ATOMIC_PRESENTATION_MODE_V2 | ML_FF_SOURCE_FRAME_ID_V1));
        resetHandshake(modern != 0, true);
        microphonePing = "too-short";
        CHECK(handshake() == 0 && MicPortNumber == 48001 && MicPingPayload.payload[0] == 0);
        const char* invalidPorts[] = {NULL, "unicast", "server_port=0", "server_port=65536", "server_port=-1"};
        for (size_t i = 0; i < sizeof(invalidPorts) / sizeof(invalidPorts[0]); i++) {
            resetHandshake(modern != 0, true);
            microphoneTransport = invalidPorts[i];
            CHECK(handshake() == 0 && MicPortNumber == 0 && microphonePlays == 0);
        }
        const int unsupportedStatuses[] = {404, 461, 500};
        for (size_t i = 0; i < sizeof(unsupportedStatuses) / sizeof(unsupportedStatuses[0]); i++) {
            resetHandshake(modern != 0, true);
            microphoneSetupStatus = unsupportedStatuses[i];
            CHECK(handshake() == 0 && MicPortNumber == 0 && microphonePlays == 0);
        }
        resetHandshake(modern != 0, true);
        failMicrophoneSetup = true;
        CHECK(handshake() == 0 && MicPortNumber == 0 && microphonePlays == 0);
        resetHandshake(modern != 0, true);
        cancelMicrophoneSetup = true;
        CHECK(handshake() != 0 && MicPortNumber == 0 && microphonePlays == 0);
    }
    resetHandshake(false, true);
    microphonePlayStatus = 461;
    CHECK(handshake() == 0 && MicPortNumber == 0 && microphonePlays == 1);
    resetHandshake(false, true);
    failMicrophonePlay = true;
    CHECK(handshake() == 0 && MicPortNumber == 0 && microphonePlays == 1);
    puts("PASS: SETUP target/port/ping, old/new PLAY, disabled/unsupported/error fallback and cancellation");
}

static void testEncryptionNegotiation(void) {
    for (int scenario = 0; scenario < 6; scenario++) {
        resetHandshake(true, scenario != 5);
        hostEncryptionSupport = scenario == 3 ? SS_ENC_AUDIO : SS_ENC_AUDIO | SS_ENC_MICROPHONE;
        StreamConfig.encryptionFlags = scenario == 1 ? ENCFLG_MICROPHONE : scenario == 2 ? 0 : ENCFLG_AUDIO;
        hostEncryptionRequest = scenario == 2 ? SS_ENC_MICROPHONE : 0;
        if (scenario == 4) StreamConfig.encryptionFlags = 0;
        CHECK(handshake() == 0);
        CHECK(((announcedEncryption & SS_ENC_MICROPHONE) != 0) == (scenario < 3));
        CHECK(announcedEncryption == EncryptionFeaturesEnabled);
    }
    puts("PASS: microphone encryption follows audio, explicit client opt-in or host request only when supported");
}

int main(void) {
    testNegotiation();
    testEncryptionNegotiation();
    puts("Microphone RTSP tests passed (production handshake/parser/SDP, controlled TCP responses).");
    return 0;
}
