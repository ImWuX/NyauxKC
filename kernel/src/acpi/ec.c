#include "ec.h"

#include <stdint.h>

#include "uacpi/acpi.h"
#include "uacpi/event.h"
#include "uacpi/kernel_api.h"
#include "uacpi/namespace.h"
#include "uacpi/opregion.h"
#include "uacpi/status.h"
#include "uacpi/tables.h"
#include "uacpi/types.h"
#include "utils/basic.h"

// Stolen from managarm AND OBOS :trl:.

#define EC_OBF	   (1 << 0)
#define EC_IBF	   (1 << 1)
#define EC_BURST   (1 << 4)
#define EC_SCI_EVT (1 << 5)

#define RD_EC 0x80
#define WR_EC 0x81
#define BE_EC 0x82
#define BD_EC 0x83
#define QR_EC 0x84

#define BURST_ACK 0x90
static uint16_t ec_gpe_idx;	   // tells u which gpe is coming from the ec
spinlock_t ec_lock;
bool ec_inited = false;
static struct acpi_gas ec_control_register;
static struct acpi_gas ec_data_register;
static uacpi_namespace_node* ec_node;
static uacpi_namespace_node* ec_gpe_node;

// this function was yoinked from obos
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
static uacpi_iteration_decision ec_enumerate_resources(void* user, uacpi_resource* resource)
{
	uint8_t* current_index = user;
	struct acpi_gas current_gas = {};
	switch (resource->type)
	{
		case UACPI_RESOURCE_TYPE_IO:
			current_gas.address = resource->io.minimum;
			current_gas.register_bit_width = resource->io.length * 8;
			break;
		case UACPI_RESOURCE_TYPE_FIXED_IO:
			current_gas.address = resource->fixed_io.address;
			current_gas.register_bit_width = resource->fixed_io.length * 8;
			break;
		default: return UACPI_ITERATION_DECISION_CONTINUE;
	}
	current_gas.address_space_id = UACPI_ADDRESS_SPACE_SYSTEM_IO;
	switch (*current_index)
	{
		case 0:
			ec_data_register = current_gas;
			(*current_index)++;
			break;
		case 1:
			ec_control_register = current_gas;
			(*current_index)++;
			break;
		default: return UACPI_ITERATION_DECISION_BREAK;
	}
	return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision ec_match(void* udata, uacpi_namespace_node* node, uint32_t unused)
{
	struct uacpi_resources* resources = NULL;
	uacpi_status status = uacpi_get_current_resources(node, &resources);
	if (uacpi_unlikely_error(status))
		return UACPI_ITERATION_DECISION_BREAK;

	uint8_t current_index = 0;
	uacpi_for_each_resource(resources, ec_enumerate_resources, &current_index);
	uacpi_free_resources(resources);

	if (current_index < 2)
		return UACPI_ITERATION_DECISION_CONTINUE;

	ec_node = node;
	ec_inited = true;
	return UACPI_ITERATION_DECISION_BREAK;
}
// end of obos yoiunking

// waits until the desired mask is reached
static void ec_wait_for_bit(struct acpi_gas* reg, uint8_t mask, uint8_t desired_mask)
{
	uint64_t val = 0;
	while ((val & mask) != desired_mask)
	{
		uacpi_status status = uacpi_gas_read(reg, &val);
		if (uacpi_unlikely_error(status))
		{
			panic("ec(): encountered an error reading the ec, uacpi_status_reason: %s\n");
		}
	}
}
static uint8_t ec_read(struct acpi_gas* ec)
{
	uint64_t val = 0;
	// waits til output buf is full lol
	if (ec != &ec_control_register)
	{
		ec_wait_for_bit(&ec_control_register, EC_OBF, EC_OBF);
	}
	uacpi_status status = uacpi_gas_read(ec, &val);
	assert(status == UACPI_STATUS_OK);

	return val;
}
static void ec_write(struct acpi_gas* ec, uint8_t val)
{
	// wait til ec input buffer is 0
	ec_wait_for_bit(&ec_control_register, EC_IBF, 0);
	uacpi_status status = uacpi_gas_write(ec, val);
	assert(status == UACPI_STATUS_OK);
}
static uint8_t ec_readreal(uint8_t offset)
{
	// send a command to the ec to read a value from its registers
	ec_write(&ec_control_register, RD_EC);
	// send the address byte in the ec data register, basically the offset
	ec_write(&ec_data_register, offset);
	return (uint8_t)ec_read(&ec_data_register);
}
static void ec_writereal(uint8_t offset, uint8_t value)
{
	// send a command to the ec to write a value to its registers
	ec_write(&ec_control_register, WR_EC);
	// send the address byte in the ec data register, basically the offset
	ec_write(&ec_data_register, offset);
	ec_write(&ec_data_register, value);
}
static bool ec_burst_time()
{
	ec_write(&ec_control_register, BE_EC);	  // enables burst mode lol
	uint8_t response = ec_read(&ec_data_register);
	if (response != BURST_ACK)
	{
		kprintf("ec(): burst enable didnt get listened to by ec because its racist towards nyaux\n");
		return false;
	}
	return true;
}
static void ec_burst_nomoretime(bool wasthereack)
{
	if (!wasthereack)
	{
		return;	   // dont do anything if there was no ack recieved from the ec
	}
	ec_write(&ec_control_register, BD_EC);				   // disables burst mode
	ec_wait_for_bit(&ec_control_register, EC_BURST, 0);	   // wait til burst mode is disabled
}
static uacpi_status ec_readuacpi(uacpi_region_rw_data* data)
{
	spinlock_lock(&ec_lock);
	bool ack = ec_burst_time();
	data->value = ec_readreal(data->offset);
	ec_burst_nomoretime(ack);
	spinlock_unlock(&ec_lock);
	return UACPI_STATUS_OK;
}
static uacpi_status ec_writeuacpi(uacpi_region_rw_data* data)
{
	spinlock_lock(&ec_lock);
	bool ack = ec_burst_time();
	ec_writereal(data->offset, data->value);
	ec_burst_nomoretime(ack);
	return UACPI_STATUS_OK;
}
static uacpi_status ecamlhandler(uacpi_region_op op, uacpi_handle data)
{
	switch (op)
	{
		case UACPI_REGION_OP_ATTACH: break;
		case UACPI_REGION_OP_DETACH: return UACPI_STATUS_OK;
		case UACPI_REGION_OP_READ:
			// do ec read
			return ec_readuacpi((uacpi_region_rw_data*)data);
		case UACPI_REGION_OP_WRITE:
			// do ec write
			return ec_writeuacpi((uacpi_region_rw_data*)data);
		default: return UACPI_STATUS_OK;
	}
}
void ecevulatemethod(uacpi_handle hand)
{
	uint8_t hnd = (uint8_t)(uint64_t)hand;
	char method[] = {'_', 'Q', 0, 0, 0};
	npf_snprintf(method + 2, 3, "%02X", hnd);
	uacpi_eval_simple(ec_node, method, NULL);
	uacpi_finish_handling_gpe(ec_gpe_node, ec_gpe_idx);
}
static bool ec_querytime(uint8_t* idx)
{
	uint8_t status = ec_read(&ec_control_register);
	if (~status & EC_SCI_EVT)
	{
		return false;
	}
	bool ack = ec_burst_time();
	ec_write(&ec_control_register, QR_EC);
	*idx = ec_read(&ec_data_register);
	ec_burst_nomoretime(ack);
	return (bool)*idx;
}
uacpi_interrupt_ret eccoolness(uacpi_handle udata, uacpi_namespace_node* gpe_dev, uint16_t gpe_idx)
{
	spinlock_lock(&ec_lock);
	uint8_t idx = 0;
	if (!ec_querytime(&idx))
	{
		return UACPI_INTERRUPT_HANDLED | UACPI_GPE_REENABLE;
	}
	uacpi_kernel_schedule_work(UACPI_WORK_GPE_EXECUTION, ecevulatemethod, (void*)(uintptr_t)idx);
	return UACPI_INTERRUPT_HANDLED;
}
static void install_ec_handlers()
{
	uacpi_install_address_space_handler(uacpi_namespace_root(), UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, ecamlhandler,
										NULL);
	uint64_t tmp = 0;
	// evulate _GPE for the gpe ec will come from
	uacpi_status status = uacpi_eval_simple_integer(ec_node, "_GPE", &tmp);
	if (uacpi_unlikely_error(status))
	{
		return;
	}
	ec_gpe_idx = tmp & 0xFFFF;
	status = uacpi_install_gpe_handler(ec_gpe_node, ec_gpe_idx, UACPI_GPE_TRIGGERING_EDGE, eccoolness, NULL);
	if (uacpi_unlikely_error(status))
	{
		panic("cannot install gpe");
	}
}
void ec_init()
{
	if (ec_inited)
	{
		return;
	}
	uacpi_find_devices("PNP0C09", ec_match, NULL);
	if (ec_inited)
	{
		install_ec_handlers();
		kprintf("ec(): device has ec!!\n");
		return;
	}
	else
	{
		kprintf("ec(): device does not have ec\n");
		return;
	}
}
void initecfromecdt()
{
	struct uacpi_table chat = {};
	uacpi_status stat = uacpi_table_find_by_signature("ECDT", &chat);
	if (stat != UACPI_STATUS_OK)
	{
		kprintf("ec(): no ecdt table found, will init from namespace\n");
		return;
	}
	struct acpi_ecdt* ok = (struct acpi_ecdt*)chat.virt_addr;
	ec_control_register = ok->ec_control;
	ec_data_register = ok->ec_data;
	ec_gpe_idx = ok->gpe_bit;
	ec_inited = true;
	install_ec_handlers();
	kprintf("ec(): found ecdt table, installed and inited ec successfully\n");
	return;
}
