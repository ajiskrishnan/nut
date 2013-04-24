

#include "main.h"
#include "serial.h"
#include<stdlib.h>
#define DRIVER_NAME	"PACE NUT driver"
#define DRIVER_VERSION	"1.06"

/* driver description structure */
upsdrv_info_t upsdrv_info = {
	DRIVER_NAME,
	DRIVER_VERSION,
	DRV_STABLE,
	{ NULL }
};

#define ENDCHAR  13	/* replies end with CR */
#define MAXTRIES 5
#define UPSDELAY 50000	/* 50 ms delay required for reliable operation */

#define SER_WAIT_SEC	3	/* allow 3.0 sec for ser_get calls */
#define SER_WAIT_USEC	2000
#define TRUE 1
#define FALSE 0
#define BAUDRATE B38400
#define DATA_LENGTH 69
#define _countof(_Array) (sizeof(_Array)/(sizeof(_Array[0])))
char buf[69];

char *command = "AD00";

struct PaceFields {

const char *name;
int len;


} pacefield[] = 
{
	{"output.L1.voltage",3},
	{"output.L2.voltage",3},
	{"output.L3.voltage",3},
	{"output.L1.current",3},
	{"output.L2.current",3},
	{"output.L3.current",3},
	{"input.voltage",3},
	{"input.current",3},
	{"input.L1.voltage",3},
	{"input.L2.voltage",3},
	{"input.L3.voltage",3},
	{"input.frequency",3},
	{"output.frequency",3},
	{"inv.flag1",2},
	{"inv.serial",9},
	{"battery.charge",3},
	{"inv.flag2",2},
	{"inv.flag3",2},
	{"solar.voltage",3},
	{"solar.current",3},
	{"solar.power",3},
	{"solar.kwh",3}

};
enum status_position
		    {
		      ERR_STATUS              = 56,
		      CHARGE_SOURCE           = 58,
		      INV_STATUS              = 59,
		      BAT_STATUS              = 60,
		      LOAD_STATUS             = 61,
		      BAT_CHRG_DISCHRG_STATUS = 63
		    };
enum err_length
		    {
		        ERR_LEN  = 2,
			CHRGE_LEN =1,
			INV_LEN   =1,
			BAT_LEN   =1,
			LOAD_LEN  =2,
			CHRG_DCHRG_LEN =1
		    };
void upsdrv_initinfo(void)
{
	
	dstate_setinfo("ups.mfr", "%s", "Enertech");
	dstate_setinfo("ups.model", "PCU  %s", "Default");
	printf("Detected %s %s on %s\n", dstate_getinfo("ups.mfr"), 
	dstate_getinfo("ups.model"), device_path);
	
}


void upsdrv_shutdown(void)
{
/*	printf("The UPS will shut down in approximately one minute.\n");

	if (ups_on_line())
		printf("The UPS will restart in about one minute.\n");
	else
		printf("The UPS will restart when power returns.\n");

	ser_send_pace(upsfd, UPSDELAY, "S01R0001\r");
*/
}
void test_command()
{
  char ch;
  	
  while(TRUE){
	
	ser_get_char(upsfd,&ch, SER_WAIT_SEC,SER_WAIT_USEC);
	printf("data is %c\n",ch);
	if(ch == '#')
	   return;
  }	
	 
}
int test_CMD()
{ 
  int i,r;
  char header[4];
      for (i=0;i<4;i++){
         header[i]=buf[i];
      }
      header[i] = '\0';
      r = strcmp(header,command);
      if(r == 0)
	return TRUE;
      else
	return FALSE;
}


void update_err_status()
{
      int i;
      int x;
      char data[2];
      for(i=0;i<ERR_LEN;i++)
      {
	 data[i] = buf[ERR_STATUS+i];
      }
      data[i] = '\0';
      x = atoi(data);
      if(x==0)
	dstate_setinfo("error.status", "%d",0);        //No error
      else
	dstate_setinfo("error.status", "%d",x);        //update error code
}

void update_charge_source_status()
{
      if( (buf[CHARGE_SOURCE]) == '0'){
	dstate_setinfo("charge.solar", "%s","OFF");   //charge on mains
      }
      else
	dstate_setinfo("charge.solar", "%s","ON");    //charge on solar
}

void update_inverter_status()
{
  if((buf[INV_STATUS]) == '0')
    dstate_setinfo("inverter.status", "%s","OFF");
  else
    dstate_setinfo("inverter.status", "%s","ON");
}

void update_battery_status()
{
  if((buf[BAT_STATUS])=='0')
    status_set("OB");                   //battery normal
  else if((buf[BAT_STATUS])=='1')
    status_set("RB");                  //battery low trip output
  else
    status_set("LB");
}

void update_load_status()
{
      int i;
      int x;
      char data[2];
      for(i=0;i<LOAD_LEN;i++)
      {
	 data[i] = buf[LOAD_STATUS+i];
      }
      data[i] = '\0';
      x = atoi(data);
      if(x == 2)
	status_set("OVER");             //over load
      else
	status_set("TRIM");             //over load trip out
}

void update_battery_charge_dchrg_status()
{
  if (buf[BAT_CHRG_DISCHRG_STATUS] == '0')
    status_set("DISCHRG");
  else
    status_set("CHRG");
}

void upsdrv_updateinfo(void)
{
	int r;
	int x,i=0,j,k,value;
	float value1;
	char ch;
	char data[10];
	int data_position = 0;
	
	test_command();
	
    	x= ser_get_buf_len(upsfd,buf, DATA_LENGTH, SER_WAIT_SEC,SER_WAIT_USEC);
	printf("reading status is %d\n",x);
	printf("the reading data is %s\n",buf);

	ser_comm_good();
	
	i = data_position;
	for(j =0;j< _countof(pacefield);j++)
	{
	   
		 for(k=0;k<pacefield[j].len;k++)
		 {
			data[k] = buf[i+k];
			
		
		 }
		 data[k] = '\0';
		 printf("the reading data is %s\n",data);
		// value = atoi(data);
		// value1 = (float)value/pacefield[j].divident;
		// printf("value 1 = %f\n",value1);
		 dstate_setinfo(pacefield[j].name,data);
		 i = i+pacefield[j].len;
	    
	}
	status_init();
//	update_err_status();
//	update_charge_source_status();
//	update_inverter_status();
//	update_battery_status();
//	update_load_status();
//	update_battery_charge_dchrg_status();
	status_commit();
	dstate_dataok();
  
	
}

void upsdrv_help(void)
{
}

void upsdrv_makevartable(void)
{
	addvar(VAR_VALUE, "solar.voltage", "solar voltage");
	addvar(VAR_VALUE, "solar.current", "solar current");
	addvar(VAR_VALUE, "solar.power", "solar power");
	addvar(VAR_VALUE, "solar.kwh", "solar KWH");	
	addvar(VAR_VALUE, "inv.flag1", "inverter flag1");
	addvar(VAR_VALUE, "inv.flag2", "inverter flag2");
	addvar(VAR_VALUE, "inv.flag3", "inverter flag3");
	addvar(VAR_VALUE, "inv.number", "serial number of the inverter");
}

void upsdrv_initups(void)
{
	int fdm, fds;
//	char *slavename;                       //only for ptmx
//	extern char *ptsname();                //only for ptmx
     
	upsfd = ser_open(device_path);
//	grantpt(upsfd);                        /* change permission of slave , only for ptmx*/
//	unlockpt(upsfd);                       /* unlock slave ,only for ptmx */
//	slavename = ptsname(upsfd);           //only for ptmx
//	printf("the slave name is %s\n",slavename);
	printf("driver initups is running");
	ser_set_speed(upsfd, device_path, BAUDRATE);
}

void upsdrv_cleanup(void)
{
	ser_close(upsfd, device_path);
}
