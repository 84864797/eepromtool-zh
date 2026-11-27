/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 *
 * Chinese localization and Windows UTF-8 console setup, 2026-11-27.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soem/soem.h"

#ifdef _WIN32
#include <windows.h>

static UINT saved_console_output_cp;

static void restore_console_output_cp(void)
{
   fflush(stdout);
   fflush(stderr);
   SetConsoleOutputCP(saved_console_output_cp);
}

static void use_utf8_console(void)
{
   saved_console_output_cp = GetConsoleOutputCP();
   if (saved_console_output_cp && saved_console_output_cp != CP_UTF8 &&
       SetConsoleOutputCP(CP_UTF8))
   {
      atexit(restore_console_output_cp);
   }
}
#endif

#define MAXBUF          524288
#define MAX_EEPROM       (65536 * 2)
#define MINBUF          128
#define CRCBUF          14
#define EEPROM_POLL_US  200

#define MODE_NONE       0
#define MODE_READBIN    1
#define MODE_READINTEL  2
#define MODE_WRITEBIN   3
#define MODE_WRITEINTEL 4
#define MODE_WRITEALIAS 5
#define MODE_INFO       6

#define MAXSLENGTH      256

uint8 ebuf[MAXBUF];
uint8 ob;
uint16 ow;
int os;
int slave;
int alias;
ec_timet tstart, tend, tdif;
int wkc;
int mode;
char sline[MAXSLENGTH];

static ecx_contextt ctx;

#define IHEXLENGTH 0x20

void calc_crc(uint8 *crc, uint8 b)
{
   int j;
   *crc ^= b;
   for (j = 0; j <= 7; j++)
   {
      if (*crc & 0x80)
         *crc = (*crc << 1) ^ 0x07;
      else
         *crc = (*crc << 1);
   }
}

uint16 SIIcrc(uint8 *buf)
{
   int i;
   uint8 crc;

   crc = 0xff;
   for (i = 0; i <= 13; i++)
   {
      calc_crc(&crc, *(buf++));
   }
   return (uint16)crc;
}

int input_bin(char *fname, int *length)
{
   FILE *fp;

   int cc = 0, c;

   fp = fopen(fname, "rb");
   if (fp == NULL)
      return 0;
   while (((c = fgetc(fp)) != EOF) && (cc < MAXBUF))
      ebuf[cc++] = (uint8)c;
   *length = cc;
   fclose(fp);

   return 1;
}

int input_intelhex(char *fname, int *start, int *length)
{
   FILE *fp;

   int c, sc, retval = 1;
   int ll, ladr, lt, sn, i, lval;
   int hstart, hlength, sum;

   fp = fopen(fname, "r");
   if (fp == NULL)
      return 0;
   hstart = MAXBUF;
   hlength = 0;
   sum = 0;
   do
   {
      memset(sline, 0x00, MAXSLENGTH);
      sc = 0;
      while (((c = fgetc(fp)) != EOF) && (c != 0x0A) && (sc < (MAXSLENGTH - 1)))
         sline[sc++] = (uint8)c;
      if ((c != EOF) && ((sc < 11) || (sline[0] != ':')))
      {
         c = EOF;
         retval = 0;
         printf("Intel HEX 文件格式无效。\n");
      }
      if (c != EOF)
      {
         sn = sscanf(sline, ":%2x%4x%2x", &ll, &ladr, &lt);
         if ((sn == 3) && ((ladr + ll) <= MAXBUF) && (lt == 0))
         {
            sum = ll + (ladr >> 8) + (ladr & 0xff) + lt;
            if (ladr < hstart) hstart = ladr;
            for (i = 0; i < ll; i++)
            {
               sn = sscanf(&sline[9 + (i << 1)], "%2x", &lval);
               ebuf[ladr + i] = (uint8)lval;
               sum += (uint8)lval;
            }
            if (((ladr + ll) - hstart) > hlength)
               hlength = (ladr + ll) - hstart;
            sum = (0x100 - sum) & 0xff;
            sn = sscanf(&sline[9 + (i << 1)], "%2x", &lval);
            if (!sn || ((sum - lval) != 0))
            {
               c = EOF;
               retval = 0;
               printf("Intel HEX 记录的校验和错误。\n");
            }
         }
      }
   } while (c != EOF);
   if (retval)
   {
      *length = hlength;
      *start = hstart;
   }
   fclose(fp);

   return retval;
}

int output_bin(char *fname, int length)
{
   FILE *fp = fopen(fname, "wb");
   size_t written;

   if (fp == NULL)
      return 0;

   written = fwrite(ebuf, 1, length, fp);
   return fclose(fp) == 0 && written == (size_t)length;
}

int output_intelhex(char *fname, int length)
{
   FILE *fp;

   int cc = 0, ll, sum, i;

   fp = fopen(fname, "w");
   if (fp == NULL)
      return 0;
   while (cc < length)
   {
      ll = length - cc;
      if (ll > IHEXLENGTH) ll = IHEXLENGTH;
      sum = ll + (cc >> 8) + (cc & 0xff);
      fprintf(fp, ":%2.2X%4.4X00", ll, cc);
      for (i = 0; i < ll; i++)
      {
         fprintf(fp, "%2.2X", ebuf[cc + i]);
         sum += ebuf[cc + i];
      }
      fprintf(fp, "%2.2X\n", (0x100 - sum) & 0xff);
      cc += ll;
   }
   fprintf(fp, ":00000001FF\n");
   fclose(fp);

   return 1;
}

static int take_eeprom_control(uint16 address)
{
   uint8 control = 2;
   if (ecx_APWR(&ctx.port, address, ECT_REG_EEPCFG, sizeof(control), &control, EC_TIMEOUTRET) <= 0)
      return 0;

   control = 0;
   return ecx_APWR(&ctx.port, address, ECT_REG_EEPCFG, sizeof(control), &control, EC_TIMEOUTRET) > 0;
}

static int wait_eeprom_ready(uint16 address, uint16 *status)
{
   osal_timert timer;
   osal_timer_start(&timer, EC_TIMEOUTEEP);
   do
   {
      if (ecx_APRD(&ctx.port, address, ECT_REG_EEPSTAT, sizeof(*status), status, EC_TIMEOUTRET) > 0)
      {
         *status = etohs(*status);
         if (!(*status & EC_ESTAT_BUSY))
            return 1;
      }
      osal_usleep(EEPROM_POLL_US);
   } while (!osal_timer_is_expired(&timer));

   return 0;
}

int eeprom_read(int slave, int start, int length)
{
   uint16 address = 1 - slave;
   uint16 status;
   int block_size;

   if (slave <= 0 || slave > ctx.slavecount || start < 0 || length <= 0 ||
       start + length > MAX_EEPROM || !take_eeprom_control(address) ||
       !wait_eeprom_ready(address, &status))
      return 0;

   block_size = (status & EC_ESTAT_R64) ? 8 : 4;
   for (int offset = start; offset < start + length; offset += block_size)
   {
      uint16 command[] = { htoes(EC_ECMD_READ), htoes(offset >> 1), 0 };
      uint8 data[8];
      int read_ok = 0;
      int remaining = start + length - offset;

      for (int attempt = 0; attempt <= EC_DEFAULTRETRIES; ++attempt)
      {
         if (attempt > 0 && !wait_eeprom_ready(address, &status))
            continue;
         if (status & EC_ESTAT_EMASK)
         {
            uint16 clear = htoes(EC_ECMD_NOP);
            if (ecx_APWR(&ctx.port, address, ECT_REG_EEPCTL, sizeof(clear), &clear, EC_TIMEOUTRET) <= 0)
               continue;
         }
         if (ecx_APWR(&ctx.port, address, ECT_REG_EEPCTL, sizeof(command), command, EC_TIMEOUTRET) <= 0)
            continue;

         osal_usleep(EEPROM_POLL_US);
         if (!wait_eeprom_ready(address, &status) || (status & EC_ESTAT_EMASK))
            continue;

         if (ecx_APRD(&ctx.port, address, ECT_REG_EEPDAT, block_size, data, EC_TIMEOUTRET) > 0)
         {
            read_ok = 1;
            break;
         }
      }
      if (!read_ok)
      {
         printf("读取 EEPROM 在字节偏移 0x%X 失败。\n", offset);
         return 0;
      }
      memcpy(ebuf + offset, data, remaining < block_size ? remaining : block_size);
   }
   return 1;
}

int eeprom_write(int slave, int start, int length)
{
   uint16 address = 1 - slave;
   int count = 0;

   if (slave <= 0 || slave > ctx.slavecount || start < 0 || (start & 1) ||
       length <= 0 || start + length > MAX_EEPROM || !take_eeprom_control(address))
      return 0;

   for (int offset = start; offset < start + length; offset += 2)
   {
      uint16 word = ebuf[offset] | ((uint16)ebuf[offset + 1] << 8);
      if (ecx_writeeepromAP(&ctx, address, offset >> 1, word, EC_TIMEOUTEEP) <= 0)
      {
         printf("\n写入 EEPROM 在字节偏移 0x%X 失败。\n", offset);
         return 0;
      }
      if (++count >= 100)
      {
         count = 0;
         printf(".");
         fflush(stdout);
      }
   }
   return 1;
}

int eeprom_writealias(int slave, int alias, uint16 crc)
{
   uint16 aiadr;
   uint8 eepctl;
   int ret;

   if ((ctx.slavecount >= slave) && (slave > 0) && (alias <= 0xffff))
   {
      aiadr = 1 - slave;
      eepctl = 2;
      ecx_APWR(&ctx.port, aiadr, ECT_REG_EEPCFG, sizeof(eepctl), &eepctl, EC_TIMEOUTRET); /* force Eeprom from PDI */
      eepctl = 0;
      ecx_APWR(&ctx.port, aiadr, ECT_REG_EEPCFG, sizeof(eepctl), &eepctl, EC_TIMEOUTRET); /* set Eeprom to master */

      ret = ecx_writeeepromAP(&ctx, aiadr, 0x04, alias, EC_TIMEOUTEEP);
      if (ret)
         ret = ecx_writeeepromAP(&ctx, aiadr, 0x07, crc, EC_TIMEOUTEEP);

      return ret;
   }

   return 0;
}

int eepromtool(char *ifname, int slave, int mode, char *fname)
{
   int w, rc = 0, estart, esize = 0, ok = 0;
   uint16 *wbuf;

   /* initialise SOEM, bind socket to ifname */
   if (ecx_init(&ctx, ifname))
   {
      printf("已打开网卡 %s。\n", ifname);

      w = 0x0000;
      wkc = ecx_BRD(&ctx.port, 0x0000, ECT_REG_TYPE, sizeof(w), &w, EC_TIMEOUTSAFE); /* detect number of slaves */
      if (wkc > 0)
      {
         ctx.slavecount = wkc;

         printf("找到 %d 个 EtherCAT 从站。\n", ctx.slavecount);
         if ((ctx.slavecount >= slave) && (slave > 0))
         {
            if ((mode == MODE_INFO) || (mode == MODE_READBIN) || (mode == MODE_READINTEL))
            {
               tstart = osal_current_time();
               if (!eeprom_read(slave, 0x0000, MINBUF))
               {
                  printf("无法读取从站的 EEPROM。\n");
                  goto close_connection;
               }
               ok = mode == MODE_INFO;

               wbuf = (uint16 *)&ebuf[0];
               printf("从站 %d 的 EEPROM 信息\n", slave);
               printf(" PDI 控制       %4.4X\n", *(wbuf + 0x00));
               printf(" PDI 配置       %4.4X\n", *(wbuf + 0x01));
               printf(" 配置别名       %4.4X\n", *(wbuf + 0x04));
               printf(" 存储的校验和   %4.4X\n", *(wbuf + 0x07));
               printf(" 计算的校验和   %4.4X\n", SIIcrc(&ebuf[0]));
               printf(" 厂商 ID        %8.8X\n", *(uint32 *)(wbuf + 0x08));
               printf(" 产品码         %8.8X\n", *(uint32 *)(wbuf + 0x0A));
               printf(" 修订号         %8.8X\n", *(uint32 *)(wbuf + 0x0C));
               printf(" 序列号         %8.8X\n", *(uint32 *)(wbuf + 0x0E));
               printf(" 邮箱协议       %4.4X\n", *(wbuf + 0x1C));
               esize = (*(wbuf + 0x3E) + 1) * 128;
               if (esize <= MAX_EEPROM)
                  printf(" 容量           %4.4X = %d 字节\n", *(wbuf + 0x3E), esize);
               else
                  printf(" 容量           %4.4X（容量字段无效或未配置）\n", *(wbuf + 0x3E));
               printf(" SII 版本       %4.4X\n", *(wbuf + 0x3F));
            }
            if ((mode == MODE_READBIN) || (mode == MODE_READINTEL))
            {
               if (esize > MAX_EEPROM)
               {
                  printf("EEPROM 容量字段无效，无法确定备份长度。\n");
                  goto close_connection;
               }
               if (esize > MINBUF && !eeprom_read(slave, MINBUF, esize - MINBUF))
                  goto close_connection;

               tend = osal_current_time();
               osal_time_diff(&tstart, &tend, &tdif);
               ok = mode == MODE_READINTEL ? output_intelhex(fname, esize) : output_bin(fname, esize);
               if (!ok)
               {
                  printf("无法保存备份文件。\n");
                  goto close_connection;
               }

               printf("\n读取操作耗时 %d 毫秒。\n", (int)(tdif.tv_sec * 1000 + tdif.tv_nsec / 1000000));
            }
            if ((mode == MODE_WRITEBIN) || (mode == MODE_WRITEINTEL))
            {
               estart = 0;
               if (mode == MODE_WRITEINTEL) rc = input_intelhex(fname, &estart, &esize);
               if (mode == MODE_WRITEBIN) rc = input_bin(fname, &esize);

               if (rc > 0)
               {
                  wbuf = (uint16 *)&ebuf[0];
                  printf("写入目标从站 %d\n", slave);
                  printf(" 厂商 ID        %8.8X\n", *(uint32 *)(wbuf + 0x08));
                  printf(" 产品码         %8.8X\n", *(uint32 *)(wbuf + 0x0A));
                  printf(" 修订号         %8.8X\n", *(uint32 *)(wbuf + 0x0C));
                  printf(" 序列号         %8.8X\n", *(uint32 *)(wbuf + 0x0E));

                  printf("正在写入");
                  fflush(stdout);
                  tstart = osal_current_time();
                  ok = eeprom_write(slave, estart, esize);
                  if (!ok)
                  {
                     printf("\nEEPROM 写入中止，板卡的数据可能已部分改变。\n");
                     goto close_connection;
                  }
                  tend = osal_current_time();
                  osal_time_diff(&tstart, &tend, &tdif);

                  printf("\n写入操作耗时 %d 毫秒。请用 -r 读回校验。\n", (int)(tdif.tv_sec * 1000 + tdif.tv_nsec / 1000000));
               }
               else
                  printf("无法读取镜像文件，已取消写入。\n");
            }
            if (mode == MODE_WRITEALIAS)
            {
               if (eeprom_read(slave, 0x0000, CRCBUF)) // read first 14 bytes
               {
                  wbuf = (uint16 *)&ebuf[0];
                  *(wbuf + 0x04) = alias;
                  if (eeprom_writealias(slave, alias, SIIcrc(&ebuf[0])))
                  {
                     printf("别名 %4.4X 已写入从站 %d。\n", alias, slave);
                     ok = 1;
                  }
                  else
                  {
                     printf("别名写入失败。\n");
                  }
               }
               else
               {
                  printf("无法读取从站的 EEPROM。\n");
               }
            }
         }
         else
         {
            printf("从站序号超出扫描范围。\n");
         }
      }
      else
      {
         printf("没有找到 EtherCAT 从站。\n");
      }
close_connection:
      printf("关闭网卡连接。\n");
      /* stop SOEM, close socket */
      ecx_close(&ctx);
   }
   else
   {
      printf("无法打开网卡 %s。请检查网卡、驱动和运行权限。\n", ifname);
   }
   return ok;
}

int main(int argc, char *argv[])
{
   int ok = 1;
#ifdef _WIN32
   use_utf8_console();
#endif
   printf("SOEM EtherCAT 主站工具\nEEPROM 读写工具（中文版）\n");

   mode = MODE_NONE;
   if (argc > 3)
   {
      slave = atoi(argv[2]);
      if ((strncmp(argv[3], "-i", sizeof("-i")) == 0)) mode = MODE_INFO;
      if (argc > 4)
      {
         if ((strncmp(argv[3], "-r", sizeof("-r")) == 0)) mode = MODE_READBIN;
         if ((strncmp(argv[3], "-ri", sizeof("-ri")) == 0)) mode = MODE_READINTEL;
         if ((strncmp(argv[3], "-w", sizeof("-w")) == 0)) mode = MODE_WRITEBIN;
         if ((strncmp(argv[3], "-wi", sizeof("-wi")) == 0)) mode = MODE_WRITEINTEL;
         if ((strncmp(argv[3], "-walias", sizeof("-walias")) == 0))
         {
            mode = MODE_WRITEALIAS;
            alias = atoi(argv[4]);
         }
      }
      if (mode == MODE_NONE)
      {
         printf("命令选项无效或缺少文件名。\n");
         ok = 0;
      }
      else
         ok = eepromtool(argv[1], slave, mode, argv[4]);
   }
   else
   {
      ec_adaptert *adapter = NULL;
      ec_adaptert *head = NULL;

      printf("用法  eepromtool 网卡 从站序号 选项 [文件名或别名]\n");
      printf("网卡名称请从下方列表复制。Windows 通常使用 \\Device\\NPF_{GUID}。\n");
      printf("从站序号按 EtherCAT 接线顺序，从 1 开始。\n");
      printf("    -i      查看 EEPROM 信息\n");
      printf("    -walias 写入从站别名\n");
      printf("    -r      读取 EEPROM，保存为 BIN\n");
      printf("    -ri     读取 EEPROM，保存为 Intel HEX\n");
      printf("    -w      从 BIN 读取镜像并写入 EEPROM\n");
      printf("    -wi     从 Intel HEX 读取镜像并写入 EEPROM\n");

      printf("\n可用网卡\n");
      head = adapter = ec_find_adapters();
      while (adapter != NULL)
      {
         printf("    - %s  (%s)\n", adapter->name, adapter->desc);
         adapter = adapter->next;
      }
      ec_free_adapters(head);
   }

   printf("程序结束。\n");

   return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
