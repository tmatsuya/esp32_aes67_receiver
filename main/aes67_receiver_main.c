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

#define MULTICAST_IPV4_ADDR	"239.69.83.134"
#define UDP_PORT		5004

#define MULTICAST_LOOPBACK	CONFIG_EXAMPLE_LOOPBACK
#define MULTICAST_TTL		CONFIG_EXAMPLE_MULTICAST_TTL

#define SDP_IPV4_ADDR		"239.255.255.255"
#define	SDP_PORT		9875

#define	SDP_RECIEVE_ENTRY_MAX	(16)

#define BUTTON_GPIO		(2)


static const char *TAG = "multicast";
static const char *V4TAG = "mcast-ipv4";

static long multicast_src_ipv4_addr_long_cur = 0;
static char multicast_dst_ipv4_addr_cur[16] = MULTICAST_IPV4_ADDR;
static int  multicast_dst_ipv4_port_cur = UDP_PORT;

static long multicast_src_ipv4_addr_long_next = 0;
static char multicast_dst_ipv4_addr_next[16] = MULTICAST_IPV4_ADDR; 
static int  multicast_dst_ipv4_port_next = UDP_PORT;

static int reload_request          = 0;
static int source_cur              = 0;

static int socket_add_ipv4_multicast_group(int sock, bool assign_source_if, char *ipv4_addr, int udp)
{
    struct ip_mreq imreq = { 0 };
    struct in_addr iaddr = { 0 };
    int err = 0;
    // Configure source interface
    imreq.imr_interface.s_addr = IPADDR_ANY;
    // Configure multicast address to listen to
    err = inet_aton(ipv4_addr, &imreq.imr_multiaddr.s_addr);
    if (err != 1) {
        ESP_LOGE(V4TAG, "Configured IPV4 multicast address '%s' is invalid.", ipv4_addr);
        // Errors in the return value have to be negative
        err = -1;
        goto err;
    }
    ESP_LOGI(TAG, "Configured IPV4 Multicast address %s", inet_ntoa(imreq.imr_multiaddr.s_addr));
    if (!IP_MULTICAST(ntohl(imreq.imr_multiaddr.s_addr))) {
        ESP_LOGW(V4TAG, "Configured IPV4 multicast address '%s' is not a valid multicast address. This will probably not work.", ipv4_addr);
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

static int create_multicast_dst_ipv4_socket(char *ipv4_addr, int udp)
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
    saddr.sin_port = htons(udp);
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
    err = socket_add_ipv4_multicast_group(sock, true, ipv4_addr, udp);
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
    int sock, i, tmp1, rc;
    static int seq_no_before = -1;
    unsigned char recvbuf[2000];
    //char raddr_name[32] = { 0 };
    int seq_no, seq_no_diff, len, old_len, err;
    struct sockaddr_storage raddr; // Large enough for IPv4
    socklen_t socklen = sizeof(raddr);
    fd_set rfds, rfds_default;
    i2s_chan_handle_t tx_handle;
    size_t bytes_written;
    int pcm_msec = 1;		// 1 or 5 msec interval
    //int rtp_payload_size;	// 204 (L16 1ms) or 300 (L24 1ms) or 972 (L16 5ms) or 1452 (L24 5ms)
    int pcm_byte_per_frame = 8;	// 8 (L24) or 4 (L16)

    //rtp_payload_size = pcm_byte_per_frame * 48 * pcm_msec;


    old_len = -1;

    while (1) {
        struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = 50 * 1000,	// 50 msec
        };

reload:
        if (reload_request) {
            multicast_src_ipv4_addr_long_cur  = multicast_src_ipv4_addr_long_next;
            strcpy(multicast_dst_ipv4_addr_cur, multicast_dst_ipv4_addr_next);
            multicast_dst_ipv4_port_cur  = multicast_dst_ipv4_port_next;
            old_len = 0;
            reload_request = 0;
        }

        sock = create_multicast_dst_ipv4_socket(multicast_dst_ipv4_addr_cur, multicast_dst_ipv4_port_cur);
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
            .sin_port = htons(multicast_dst_ipv4_port_cur),
        };
        // We know this inet_aton will pass because we did it above already
        inet_aton(multicast_dst_ipv4_addr_cur, &sdestv4.sin_addr.s_addr);

        FD_ZERO(&rfds_default);
        FD_SET(sock, &rfds_default);

        do {
            memcpy(&rfds, &rfds_default, sizeof(fd_set));
            rc = select(sock + 1, &rfds, NULL, NULL, &tv);
            if (rc < 0) {
                ESP_LOGE(TAG, "Select failed: errno %d", errno);
            }
            if (reload_request) {
                shutdown(sock, 0);
                close(sock);
                goto reload;
            }
        } while (rc == 0);
        do {
            len = recvfrom(sock, recvbuf, sizeof(recvbuf)-1, 0, (struct sockaddr *)&raddr, &socklen);
        } while (multicast_src_ipv4_addr_long_cur != 0 && (((struct sockaddr_in *)&raddr)->sin_addr.s_addr) != multicast_src_ipv4_addr_long_cur);
        if (multicast_src_ipv4_addr_long_cur == 0 && len >= 0) {
            multicast_src_ipv4_addr_long_cur = (((struct sockaddr_in *)&raddr)->sin_addr.s_addr);
        }
//ESP_LOGI(TAG,"s_addr=%lX", (((struct sockaddr_in *)&raddr)->sin_addr.s_addr));
        if ( len != old_len ) {
            switch (len) {
                case 204:  pcm_byte_per_frame = 4; pcm_msec = 1;  // L16 1ms
                    break;
                case 300:  pcm_byte_per_frame = 8; pcm_msec = 1;  // L24 1ms
                    break;
                case 972:  pcm_byte_per_frame = 4; pcm_msec = 5;  // L16 5ms
                    break;
                case 1452: pcm_byte_per_frame = 8; pcm_msec = 5;  // L24 5ms
                    break;
            }

            i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
            //i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
            i2s_new_channel(&chan_cfg, &tx_handle, NULL);


            i2s_std_config_t std16_cfg = {
                .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
                .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    //            .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
                .gpio_cfg = {
//                    .mclk = I2S_GPIO_UNUSED,
                    .mclk = GPIO_NUM_0,
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

            i2s_std_config_t std32_cfg = {
                .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
                .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    //            .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
                .gpio_cfg = {
//                    .mclk = I2S_GPIO_UNUSED,
                    .mclk = GPIO_NUM_0,
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
            if (pcm_byte_per_frame == 4) {
                i2s_channel_init_std_mode(tx_handle, &std16_cfg);
            } else {
                i2s_channel_init_std_mode(tx_handle, &std32_cfg);
            }

            /* Before write data, start the tx channel first */
            i2s_channel_enable(tx_handle);

            seq_no_before = -1;
            old_len = len;
        }


        // Loop waiting for UDP received, and sending UDP packets if we don't
        // see any.
        err = 1;
    
        while (err > 0) {
            len = recvfrom(sock, recvbuf, sizeof(recvbuf)-1, 0, (struct sockaddr *)&raddr, &socklen);
            if (len > 0) {
//ESP_LOGI(TAG,"s_addr=%lX", (((struct sockaddr_in *)&raddr)->sin_addr.s_addr));
                if ((((struct sockaddr_in *)&raddr)->sin_addr.s_addr) != multicast_src_ipv4_addr_long_cur)
                    continue;
                if (len != old_len || reload_request != 0)
                    break;
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
                if (pcm_byte_per_frame == 8) {
                    for (i=(48*pcm_msec*2-1); i>=0; --i) {
                        tmp1            = recvbuf[14+i*3];
                        recvbuf[15+i*4] = recvbuf[12+i*3];
                        recvbuf[14+i*4] = recvbuf[13+i*3];
                        recvbuf[13+i*4] = tmp1;
                        recvbuf[12+i*4] = 0;
                    }
                } else if (pcm_byte_per_frame == 4) {
                    for (i=0; i<pcm_byte_per_frame*48*pcm_msec; i+=pcm_byte_per_frame>>1) {
                        tmp1          = recvbuf[12+i];
                        recvbuf[12+i] = recvbuf[13+i];
                        recvbuf[13+i] = tmp1;
                    }
                }
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

        /* Have to stop the channel before deleting it */
        i2s_channel_disable(tx_handle);
        /* If the handle is not needed any more, delete it to release the channel resources */
        i2s_del_channel(tx_handle);

        ESP_LOGE(TAG, "Shutting down socket and restarting...");
        shutdown(sock, 0);
        close(sock);

    }

}

static void manage_example_task(void *pvParameters)
{
    int sock, i, rc, dst_ipv4_port;
    int button, button_before;
    unsigned char recvbuf[384], *p;
    struct _sdp {
        char src_ipv4_addr[16];
        long src_ipv4_addr_long;
        char dst_ipv4_addr[16];
        int  dst_ipv4_port;
    } sdp_table[SDP_RECIEVE_ENTRY_MAX];
    int sdp_table_max;
    char p1 = 0, p2[64], dst_ipv4_addr[16], src_ipv4_addr[16];
    long src_ipv4_addr_long;
    //char raddr_name[32] = { 0 };
    int len, err;
    struct sockaddr_storage raddr; // Large enough for IPv4
    socklen_t socklen = sizeof(raddr);
    fd_set rfds, rfds_default;

    esp_rom_gpio_pad_select_gpio(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY);  //PULLUP が必要  

    //rtp_payload_size = pcm_byte_per_frame * 48 * pcm_msec;

    button = button_before = 0;
    source_cur = 0;

    sdp_table_max = 1;
    sdp_table[0].src_ipv4_addr_long = 0;
    strcpy( sdp_table[0].dst_ipv4_addr, MULTICAST_IPV4_ADDR);
    sdp_table[0].dst_ipv4_port = UDP_PORT;

    while (1) {
        struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = 50 * 1000,	// 50 msec
        };

        sock = create_multicast_dst_ipv4_socket(SDP_IPV4_ADDR, SDP_PORT);
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
            .sin_port = htons(SDP_PORT),
        };
        // We know this inet_aton will pass because we did it above already
        inet_aton(SDP_IPV4_ADDR, &sdestv4.sin_addr.s_addr);

        FD_ZERO(&rfds_default);
        FD_SET(sock, &rfds_default);

        // Loop waiting for UDP received, and sending UDP packets if we don't
        // see any.
        err = 1;

        while (err > 0) {
            memcpy(&rfds, &rfds_default, sizeof(fd_set));
            rc = select(sock + 1, &rfds, NULL, NULL, &tv);
            if (rc == 0) {
                button = !gpio_get_level(BUTTON_GPIO);
                if (button == 1 && button_before == 0) {
                    // search next SDP entry
                    if (++source_cur >= sdp_table_max) {
                        source_cur = 0;
                    }
                    multicast_src_ipv4_addr_long_next = sdp_table[source_cur].src_ipv4_addr_long;
                    strcpy(multicast_dst_ipv4_addr_next, sdp_table[source_cur].dst_ipv4_addr);
                    multicast_dst_ipv4_port_next = sdp_table[source_cur].dst_ipv4_port;
                    reload_request = 1;
                    ESP_LOGI(TAG, "Button pushed. Source=%d", source_cur);
                }            
                button_before = button;
                continue;
            }
            if (rc < 0) {
                ESP_LOGE(TAG, "Select failed: errno %d", errno);
            }
            len = recvfrom(sock, recvbuf, sizeof(recvbuf)-1, 0, (struct sockaddr *)&raddr, &socklen);
            if (len > 0) {
                recvbuf[len] = '\0';
                if (!strcmp( (char *)recvbuf+8, "application/sdp") ) {
                    ESP_LOGI(TAG, "Recieved SDP");
                    for (p = recvbuf+24; p < (recvbuf+len); ++p) {
                        if (*(p+1) == '=') {
                            p1 = *p;
                            p += 2;
                            for (i=0; i<sizeof(p2) && *p != '\r'; ++i, ++p)
                                p2[i] = *p;
                            p2[i] = '\0';
                        }
                        if (*p == '\r' || *p == '\0') {
                            if (p1 == 'c') {		// Connection information
                                char *p0;
                                rc = sscanf(p2, "IN IP4 %s", dst_ipv4_addr);
                                for (p0 = dst_ipv4_addr; p0 < (dst_ipv4_addr+strlen(dst_ipv4_addr)); ++p0)
                                    if (*p0 == '/') *p0 = '\0';
                                ESP_LOGI(TAG, "Found Multicast addr:%s", dst_ipv4_addr);
                            } else if (p1 == 'm') {	// Media name and transport
                                rc = sscanf(p2, "audio %d", &dst_ipv4_port);
                                ESP_LOGI(TAG, "Found Multicast port:%d", dst_ipv4_port);
                            } else if (p1 == 'o') {	// Origin
                                char *p0;
                                p0 = strstr(p2, "IP4 ");
                                if (p0) {
                                    rc = sscanf(p0, "IP4 %s", src_ipv4_addr);
                                    if (rc == 1) {
                                        inet_pton(AF_INET, src_ipv4_addr, (void *)&src_ipv4_addr_long);
                                    }
                                }
                                ESP_LOGI(TAG, "Found Source IP addr:%lX", src_ipv4_addr_long);
                            } else {
//                                ESP_LOGI(TAG, "%c->%s", p1, p2);
                            }
                        }
                    }
                    // record to SDP table entry
                    for (i=0; i<sdp_table_max; ++i) {
                        if (sdp_table[i].src_ipv4_addr_long == src_ipv4_addr_long && !strcmp(sdp_table[i].dst_ipv4_addr, dst_ipv4_addr) && sdp_table[i].dst_ipv4_port == dst_ipv4_port)
                            break;
                    }
                    if (i == sdp_table_max && i < SDP_RECIEVE_ENTRY_MAX) {
                        strcpy(sdp_table[sdp_table_max].src_ipv4_addr, src_ipv4_addr);
                        sdp_table[sdp_table_max].src_ipv4_addr_long = src_ipv4_addr_long;
                        strcpy(sdp_table[sdp_table_max].dst_ipv4_addr, dst_ipv4_addr);
                        sdp_table[sdp_table_max].dst_ipv4_port = dst_ipv4_port;
                        ++sdp_table_max;
                    }
                }
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

    //xTaskCreate(&mcast_example_task, "mcast_task", 4096, NULL, 5, NULL);
    xTaskCreatePinnedToCore(&mcast_example_task, "mcast_task", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(&manage_example_task, "manage_task", 4096, NULL, 5, NULL, 1);

}
