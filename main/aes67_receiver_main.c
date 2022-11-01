/* UDP MultiCast Send/Receive Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"


#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "driver/i2s_std.h"
#include "driver/gpio.h"

//#define TO_L16			// L24 to L16 convert

#define UDP_PORT		5004
#define MULTICAST_LOOPBACK	CONFIG_EXAMPLE_LOOPBACK
#define MULTICAST_TTL		CONFIG_EXAMPLE_MULTICAST_TTL
#define MULTICAST_IPV4_ADDR	"239.69.83.134"

static const char *TAG = "multicast";
static const char *V4TAG = "mcast-ipv4";


static int socket_add_ipv4_multicast_group(int sock, bool assign_source_if)
{
    struct ip_mreq imreq = { 0 };
    struct in_addr iaddr = { 0 };
    int err = 0;
    // Configure source interface
    imreq.imr_interface.s_addr = IPADDR_ANY;
    // Configure multicast address to listen to
    err = inet_aton(MULTICAST_IPV4_ADDR, &imreq.imr_multiaddr.s_addr);
    if (err != 1) {
        ESP_LOGE(V4TAG, "Configured IPV4 multicast address '%s' is invalid.", MULTICAST_IPV4_ADDR);
        // Errors in the return value have to be negative
        err = -1;
        goto err;
    }
    ESP_LOGI(TAG, "Configured IPV4 Multicast address %s", inet_ntoa(imreq.imr_multiaddr.s_addr));
    if (!IP_MULTICAST(ntohl(imreq.imr_multiaddr.s_addr))) {
        ESP_LOGW(V4TAG, "Configured IPV4 multicast address '%s' is not a valid multicast address. This will probably not work.", MULTICAST_IPV4_ADDR);
    }

    if (assign_source_if) {
        // Assign the IPv4 multicast source interface, via its IP
        // (only necessary if this socket is IPV4 only)
        err = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &iaddr,
                         sizeof(struct in_addr));
        if (err < 0) {
            ESP_LOGE(V4TAG, "Failed to set IP_MULTICAST_IF. Error %d", errno);
            goto err;
        }
    }

    err = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         &imreq, sizeof(struct ip_mreq));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to set IP_ADD_MEMBERSHIP. Error %d", errno);
        goto err;
    }

 err:
    return err;
}

static int create_multicast_ipv4_socket(void)
{
    struct sockaddr_in saddr = { 0 };
    int sock = -1;
    int err = 0;

    sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(V4TAG, "Failed to create socket. Error %d", errno);
        return -1;
    }

    // Bind the socket to any address
    saddr.sin_family = PF_INET;
    saddr.sin_port = htons(UDP_PORT);
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    err = bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to bind socket. Error %d", errno);
        goto err;
    }


    // Assign multicast TTL (set separately from normal interface TTL)
    uint8_t ttl = MULTICAST_TTL;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to set IP_MULTICAST_TTL. Error %d", errno);
        goto err;
    }

#if MULTICAST_LOOPBACK
    // select whether multicast traffic should be received by this device, too
    // (if setsockopt() is not called, the default is no)
    uint8_t loopback_val = MULTICAST_LOOPBACK;
    err = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP,
                     &loopback_val, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to set IP_MULTICAST_LOOP. Error %d", errno);
        goto err;
    }
#endif

    // this is also a listening socket, so add it to the multicast
    // group for listening...
    err = socket_add_ipv4_multicast_group(sock, true);
    if (err < 0) {
        goto err;
    }

    // All set, socket is configured for sending and receiving
    return sock;

err:
    close(sock);
    return -1;
}

static void mcast_example_task(void *pvParameters)
{
    int sock, i, tmp1, tmp2, tmp3;
    static int seq_no_before = -1;
    unsigned char recvbuf[2000];
    //char raddr_name[32] = { 0 };
    int seq_no, seq_no_diff, len, err;
    struct sockaddr_storage raddr; // Large enough for IPv4
    socklen_t socklen = sizeof(raddr);
    i2s_chan_handle_t tx_handle;
    size_t bytes_written;
    int pcm_msec;		// 1 or 5 msec interval
    //int rtp_payload_size;	// 300 (L24 1ms) or 972 (L16 5ms)
    int pcm_byte_per_frame;	// 6 (L24) or 4 (L16)


#ifdef TO_L16			// L24 to L16 convert
    pcm_byte_per_frame = 4;
    pcm_msec = 5;
#else				// no convert
    pcm_byte_per_frame = 8;
    pcm_msec = 5;
#endif
    //rtp_payload_size = pcm_byte_per_frame * 48 * pcm_msec;


    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    //i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, &tx_handle, NULL);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
#ifdef TO_L16			// L24 to L16 convert
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
#else
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
#endif
    //    .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_NUM_26,
            .ws = GPIO_NUM_22,
            .dout = GPIO_NUM_25,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    /* Initialize the channel */
    i2s_channel_init_std_mode(tx_handle, &std_cfg);

    /* Before write data, start the tx channel first */
    i2s_channel_enable(tx_handle);

    while (1) {

        sock = create_multicast_ipv4_socket();
        if (sock < 0) {
            ESP_LOGE(TAG, "Failed to create IPv4 multicast socket");
        }

        if (sock < 0) {
            // Nothing to do!
            vTaskDelay(5 / portTICK_PERIOD_MS);
            continue;
        }

        // set destination multicast addresses for sending from these sockets
        struct sockaddr_in sdestv4 = {
            .sin_family = PF_INET,
            .sin_port = htons(UDP_PORT),
        };
        // We know this inet_aton will pass because we did it above already
        inet_aton(MULTICAST_IPV4_ADDR, &sdestv4.sin_addr.s_addr);


        // Loop waiting for UDP received, and sending UDP packets if we don't
        // see any.
        err = 1;
        while (err > 0) {
            len = recvfrom(sock, recvbuf, sizeof(recvbuf)-1, 0,
                               (struct sockaddr *)&raddr, &socklen);
            if (len > 0) {
                //seq_no = ntohs(*(unsigned short *)(recvbuf+2));
                seq_no =(recvbuf[2] << 8) | (recvbuf[3]);
                seq_no_diff = seq_no - seq_no_before;
//                if (seq_no_diff > 65535)
//                    seq_no_diff -= 65536;
//                if (seq_no_diff < 0)
//                    seq_no_diff += 65536;
                if (seq_no_diff != 1 && seq_no_before != -1) {
                    ESP_LOGI(TAG, "seq_no=%d(%d), drop=%d, len=%d", seq_no, seq_no_before, (seq_no_diff-1), len);
                }
                seq_no_before = seq_no;
                // swap byte order
#ifdef TO_L16			// L24 to L16 convert
                for (i=0; i<pcm_byte_per_frame*48*pcm_msec; i+=(pcm_byte_per_frame>>1)) {
                    tmp1          = recvbuf[12+i];
                    recvbuf[12+i] = recvbuf[13+i];
                    recvbuf[13+i] = tmp1;
                }
#else
                for (i=(48*pcm_msec*2-1); i>=0; --i) {
                    tmp1            = recvbuf[12+i*3];
                    tmp2            = recvbuf[13+i*3];
                    tmp3            = recvbuf[14+i*3];
                    recvbuf[12+i*4] = 0;
                    recvbuf[13+i*4] = tmp3;
                    recvbuf[14+i*4] = tmp2;
                    recvbuf[15+i*4] = tmp1;
                }
#endif
                i2s_channel_write(tx_handle, recvbuf+12, pcm_byte_per_frame*48*pcm_msec, &bytes_written, 1000);
            } else {
                ESP_LOGE(TAG, "multicast recvfrom failed: errno %d", errno);
                err = -1;
                break;
            }

            // Get the sender's address as a string
            //if (raddr.ss_family == PF_INET) {
            //    inet_ntoa_r(((struct sockaddr_in *)&raddr)->sin_addr,
            //                raddr_name, sizeof(raddr_name)-1);
            //}
            //ESP_LOGI(TAG, "received %d bytes from %s:", len, raddr_name);
            //ESP_LOGI(TAG, "seq=%d", seq_no);

        }

        ESP_LOGE(TAG, "Shutting down socket and restarting...");
        shutdown(sock, 0);
        close(sock);
    }

}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    xTaskCreate(&mcast_example_task, "mcast_task", 4096, NULL, 5, NULL);
    //xTaskCreatePinnedToCore(&mcast_example_task, "Task0", 4096, NULL, 5, NULL, 1);

}
