/*
 * Copyright (C) 2026 Xiang W <wangxiang@iscas.ac.cn>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 *
 */
#include "fwts.h"

#if defined(FWTS_HAS_ACPI) && (FWTS_ARCH_RISCV)

#include "fwts_acpi_object_eval.h"

/* Standard AML IDs for an ECAM-capable PCI host bridge. */
#define CID_PCI			"PNP0A03"
#define HID_ECAM		"PNP0A08"

/* ACPI Time and Alarm Device, same HID as src/acpi/devices/time/time.c. */
#define HID_TAD			"ACPI000E"
#define HID_PLIC		"RSCV0001"
#define HID_APLIC		"RSCV0002"
#define HID_UART		"RSCV0003"

/* Device Properties UUID: daffd814-6eba-4d8c-8a91-bc9bbf4aa301 */
static const uint8_t dsd_devprop_uuid[16] = {
	0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d,
	0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01
};

static bool no_osbus_rtc;

static int method_brsi_init(fwts_framework *fw)
{
	if (fwts_acpi_init(fw) != FWTS_OK) {
		fwts_log_error(fw, "Cannot initialise ACPI.");
		return FWTS_ERROR;
	}

	return FWTS_OK;
}

static int method_brsi_deinit(fwts_framework *fw)
{
	return fwts_acpi_deinit(fw);
}

typedef struct {
	fwts_framework *fw;
	bool found;
	bool passed;
	char device_path[128];
} method_brsi_crs_ctx;

static ACPI_STATUS method_brsi_crs_resource(ACPI_RESOURCE *resource, void *context)
{
	method_brsi_crs_ctx *ctx = context;
	const char *desc = NULL;
	uint64_t min;
	uint64_t length;

	switch (resource->Type) {
	case ACPI_RESOURCE_TYPE_IO:
		desc = "IO";
		min = resource->Data.Io.Minimum;
		length = resource->Data.Io.AddressLength;
		break;
	case ACPI_RESOURCE_TYPE_FIXED_IO:
		desc = "FixedIO";
		min = resource->Data.FixedIo.Address;
		length = resource->Data.FixedIo.AddressLength;
		break;
	case ACPI_RESOURCE_TYPE_ADDRESS16:
		if (resource->Data.Address16.ResourceType == ACPI_IO_RANGE) {
			desc = "WordIO";
			min = resource->Data.Address16.Address.Minimum;
			length = resource->Data.Address16.Address.AddressLength;
		}
		break;
	case ACPI_RESOURCE_TYPE_ADDRESS32:
		if (resource->Data.Address32.ResourceType == ACPI_IO_RANGE) {
			desc = "DWordIO";
			min = resource->Data.Address32.Address.Minimum;
			length = resource->Data.Address32.Address.AddressLength;
		}
		break;
	case ACPI_RESOURCE_TYPE_ADDRESS64:
		if (resource->Data.Address64.ResourceType == ACPI_IO_RANGE) {
			desc = "QWordIO";
			min = resource->Data.Address64.Address.Minimum;
			length = resource->Data.Address64.Address.AddressLength;
		}
		break;
	case ACPI_RESOURCE_TYPE_EXTENDED_ADDRESS64:
		if (resource->Data.ExtAddress64.ResourceType == ACPI_IO_RANGE) {
			desc = "ExtendedIO";
			min = resource->Data.ExtAddress64.Address.Minimum;
			length = resource->Data.ExtAddress64.Address.AddressLength;
		}
		break;
	default:
		break;
	}

	if (desc) {
		ctx->passed = false;
		fwts_log_info(ctx->fw,
			"PCIe Root Complex %s _CRS has I/O range descriptor %s "
			"(min=0x%" PRIx64 ", length=0x%" PRIx64 ").",
			ctx->device_path[0] ? ctx->device_path : "(unknown)",
			desc, min, length);
	}

	return AE_OK;
}

static void method_brsi_acpi_fullname(ACPI_HANDLE handle, char *out, size_t out_size)
{
	ACPI_BUFFER buf;

	if (!out || !out_size)
		return;

	buf.Pointer = out;
	buf.Length = out_size;
	out[0] = '\0';
	if (ACPI_FAILURE(AcpiGetName(handle, ACPI_FULL_PATHNAME, &buf)))
		strncpy(out, "(unknown)", out_size - 1);
}

static ACPI_STATUS method_brsi_pci_host_walk(
	ACPI_HANDLE handle,
	UINT32 nesting_level,
	void *context,
	void **return_value)
{
	method_brsi_crs_ctx *ctx = context;
	ACPI_STATUS status;

	FWTS_UNUSED(nesting_level);
	FWTS_UNUSED(return_value);

	ctx->found = true;
	method_brsi_acpi_fullname(handle, ctx->device_path, sizeof(ctx->device_path));

	fwts_log_info(ctx->fw,
		"Checking _CRS of PCIe Root Complex %s (HID %s / CID %s).",
		ctx->device_path, HID_ECAM, CID_PCI);

	status = AcpiWalkResources(handle, METHOD_NAME__CRS,
		method_brsi_crs_resource, ctx);
	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND)
		fwts_log_warning(ctx->fw,
			"Failed to walk _CRS of %s: %s.",
			ctx->device_path, AcpiFormatException(status));

	return AE_OK;
}

static int method_brsi_aml010(fwts_framework *fw)
{
	method_brsi_crs_ctx ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.fw = fw;
	ctx.found = false;
	ctx.passed = true;

	/*
	 * AcpiGetDevices() matches the given ID against both _HID and _CID,
	 * so PNP0A08 covers ECAM PCI Express host bridges.
	 */
	AcpiGetDevices(HID_ECAM, method_brsi_pci_host_walk, &ctx, NULL);

	if (!ctx.found) {
		fwts_skipped(fw,
			"AML_010: no PCIe Root Complex with _HID/_CID %s found; "
			"skipping I/O range check.",
			HID_ECAM);
		return FWTS_OK;
	}

	if (ctx.passed) {
		fwts_passed(fw,
			"AML_010: _CRS of PCIe Root Complex device(s) does not "
			"return I/O range descriptors.");
	} else {
		fwts_warning(fw,
			"AML_010: PCIe Root Complex _CRS returns I/O range "
			"descriptors. BRS-I says _CRS SHOULD NOT return I/O "
			"ranges (WordIO, DWordIO, QWordIO, IO, FixedIO or "
			"ExtendedIO).");
		fwts_advice(fw,
			"Legacy PCI I/O BARs are uncommon on modern PCIe devices "
			"and describing I/O space can complicate Root Complex "
			"configuration. Remove I/O descriptors from the Root "
			"Complex _CRS unless a specific device requires them.");
	}

	return FWTS_OK;
}

static int method_brsi_aml020(fwts_framework *fw)
{
	fwts_list *methods;
	fwts_list_link *item;
	bool found = false;

	if ((methods = fwts_acpi_object_get_names()) != NULL) {
		fwts_list_foreach(item, methods) {
			char *name = fwts_list_data(char *, item);
			size_t len;
			ACPI_HANDLE handle;
			ACPI_OBJECT_TYPE type;
			ACPI_STATUS status;
			const char *which;

			if (name == NULL)
				continue;

			len = strlen(name);
			if (len < 4)
				continue;

			if (strncmp(name + len - 4, "_PRS", 4) == 0)
				which = "_PRS";
			else if (strncmp(name + len - 4, "_SRS", 4) == 0)
				which = "_SRS";
			else
				continue;

			status = AcpiGetHandle(NULL, name, &handle);
			if (ACPI_FAILURE(status))
				continue;

			status = AcpiGetType(handle, &type);
			if (ACPI_FAILURE(status))
				continue;

			/* Skip namespace scopes that happen to end in the same suffix. */
			if (type == ACPI_TYPE_LOCAL_SCOPE)
				continue;

			found = true;
			fwts_log_info(fw, "AML_020: found %s method %s.", which, name);
		}
	}

	if (!found) {
		fwts_passed(fw,
			"AML_020: no _PRS or _SRS methods are implemented.");
	} else {
		fwts_warning(fw,
			"AML_020: _PRS and/or _SRS methods are implemented. "
			"BRS-I says these methods SHOULD NOT be implemented.");
		fwts_advice(fw,
			"ACPI resource descriptors are typically used for "
			"devices with fixed resource ranges. Flexible resource "
			"assignment via _PRS/_SRS is not supported by most "
			"modern ACPI operating systems. Remove these methods "
			"unless a device truly requires runtime rebalancing.");
	}

	return FWTS_OK;
}

static int method_brsi_aml030(fwts_framework *fw)
{
	fwts_list *methods;
	fwts_list_link *item;
	unsigned int under_sb = 0, under_pr = 0, elsewhere = 0;

	if ((methods = fwts_acpi_object_get_names()) != NULL) {
		fwts_list_foreach(item, methods) {
			char *name = fwts_list_data(char *, item);
			ACPI_HANDLE handle;
			ACPI_DEVICE_INFO *info;
			ACPI_STATUS status;

			if (name == NULL)
				continue;

			status = AcpiGetHandle(NULL, name, &handle);
			if (ACPI_FAILURE(status))
				continue;

			status = AcpiGetObjectInfo(handle, &info);
			if (ACPI_FAILURE(status))
				continue;

			if (!(info->Valid & ACPI_VALID_HID) ||
			    info->HardwareId.String == NULL ||
			    strcmp(info->HardwareId.String, "ACPI0007") != 0) {
				ACPI_FREE(info);
				continue;
			}
			ACPI_FREE(info);

			fwts_log_info(fw, "AML_030: found ACPI0007 at %s.", name);

			if (strncmp(name, "\\_SB", 4) == 0)
				under_sb++;
			else if (strncmp(name, "\\_PR", 4) == 0)
				under_pr++;
			else
				elsewhere++;
		}
	}

	if (under_pr || elsewhere) {
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_030",
			"Per-hart ACPI0007 objects must be under \\_SB, "
			"not deprecated \\_PR (%u under \\_PR, %u elsewhere, "
			"%u under \\_SB).",
			under_pr, elsewhere, under_sb);
	} else if (under_sb) {
		fwts_passed(fw,
			"AML_030: ACPI0007 per-hart objects are under \\_SB.");
	} else {
		fwts_skipped(fw,
			"AML_030: no ACPI0007 per-hart objects found.");
	}

	return FWTS_OK;
}

typedef struct {
	fwts_framework *fw;
	const char *id;
	unsigned int found;
	unsigned int failed;
} method_brsi_tad_ctx;

/*
 * Evaluate `name` under `handle`.
 * Optional `args` / `arg_count` are passed through (used by _SRT).
 *
 * On success:
 *   Integer             -> *value = Integer.Value
 *   Buffer named "_GRT" -> *value = Buffer.Length
 */
static bool method_brsi_eval(
	ACPI_HANDLE handle,
	char *name,
	ACPI_OBJECT *args,
	UINT32 arg_count,
	uint64_t *value)
{
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_OBJECT_LIST arg_list;
	ACPI_OBJECT *obj;
	ACPI_STATUS status;
	bool rc = false;

	if (args && arg_count) {
		arg_list.Count = arg_count;
		arg_list.Pointer = args;
		status = AcpiEvaluateObject(handle, name, &arg_list, &buf);
	} else {
		status = AcpiEvaluateObject(handle, name, NULL, &buf);
	}
	if (ACPI_FAILURE(status) || buf.Pointer == NULL)
		return false;

	obj = buf.Pointer;
	if (obj->Type == ACPI_TYPE_INTEGER) {
		rc = true;
		if (value)
			*value = obj->Integer.Value;
	} else if (obj->Type == ACPI_TYPE_BUFFER && !strcmp(name, "_GRT")) {
		rc = true;
		if (value)
			*value = obj->Buffer.Length;
	}

	ACPI_FREE(buf.Pointer);
	return rc;
}

static ACPI_STATUS method_brsi_tad_walk(
	ACPI_HANDLE handle,
	UINT32 nesting_level,
	void *context,
	void **return_value)
{
	method_brsi_tad_ctx *ctx = context;
	char device_path[128];
	fwts_acpi_time_buffer real_time;
	ACPI_OBJECT arg0;
	uint64_t gcp = 0;
	uint64_t grt_len = 0;
	uint64_t srt = 0;
	bool failed = false;

	FWTS_UNUSED(nesting_level);
	FWTS_UNUSED(return_value);

	ctx->found++;

	method_brsi_acpi_fullname(handle, device_path, sizeof(device_path));

	fwts_log_info(ctx->fw, "%s: found TAD %s (HID %s).",
		ctx->id, device_path, HID_TAD);

	if (!method_brsi_eval(handle, "_GCP", NULL, 0, &gcp)) {
		fwts_log_info(ctx->fw,
			"%s: %s._GCP is mandatory but missing, failed "
			"to evaluate, or did not return an Integer.",
			ctx->id, device_path);
		failed = true;
	} else {
		fwts_log_info(ctx->fw, "%s: %s._GCP returned 0x%" PRIx64 ".",
			ctx->id, device_path, gcp);
		if (gcp & ~0x1ff) {
			fwts_log_info(ctx->fw,
				"%s: %s._GCP reserved bits 9..31 are set.",
				ctx->id, device_path);
			failed = true;
		} else if (!(gcp & 0x4)) {
			fwts_log_info(ctx->fw,
				"%s: %s._GCP bit 2 (get/set real time) is not set.",
				ctx->id, device_path);
			failed = true;
		}
	}

	if (!method_brsi_eval(handle, "_GRT", NULL, 0, &grt_len)) {
		fwts_log_info(ctx->fw,
			"%s: %s._GRT is mandatory but missing, failed "
			"to evaluate, or did not return a Buffer.",
			ctx->id, device_path);
		failed = true;
	} else if (grt_len != sizeof(fwts_acpi_time_buffer)) {
		fwts_log_info(ctx->fw,
			"%s: %s._GRT returned a Buffer of %" PRIu64
			" bytes, expected %zu.",
			ctx->id, device_path, grt_len,
			sizeof(fwts_acpi_time_buffer));
		failed = true;
	} else {
		fwts_log_info(ctx->fw,
			"%s: %s._GRT returned a %" PRIu64 "-byte time buffer.",
			ctx->id, device_path, grt_len);
	}

	memset(&real_time, 0, sizeof(real_time));
	real_time.year = 2000;
	real_time.month = 1;
	real_time.day = 1;
	real_time.hour = 0;
	real_time.minute = 0;
	real_time.milliseconds = 1;
	real_time.timezone = 0;

	arg0.Type = ACPI_TYPE_BUFFER;
	arg0.Buffer.Length = sizeof(real_time);
	arg0.Buffer.Pointer = (void *)&real_time;

	if (!method_brsi_eval(handle, "_SRT", &arg0, 1, &srt)) {
		fwts_log_info(ctx->fw,
			"%s: %s._SRT is mandatory but missing, failed "
			"to evaluate, or did not return an Integer.",
			ctx->id, device_path);
		failed = true;
	} else {
		fwts_log_info(ctx->fw, "%s: %s._SRT returned 0x%" PRIx64 ".",
			ctx->id, device_path, srt);
	}

	if (failed)
		ctx->failed++;

	return AE_OK;
}

static int method_brsi_aml060(fwts_framework *fw)
{
	method_brsi_tad_ctx ctx;

	/*
	 * AML_060 applies only when the platform has an RTC on a bus the
	 * OS manages (I2C, SPI, ...). That cannot be discovered from ACPI,
	 * so --brs-i-no-osbus-rtc declares that TAD is not required.
	 */
	if (no_osbus_rtc) {
		fwts_skipped(fw,
			"AML_060: --brs-i-no-osbus-rtc specified; no RTC on "
			"an OS-managed bus, Time and Alarm Device is not "
			"required.");
		return FWTS_OK;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.fw = fw;
	ctx.id = "AML_060";

	AcpiGetDevices(HID_TAD, method_brsi_tad_walk, &ctx, NULL);

	if (ctx.found == 0)
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_060",
			"No Time and Alarm Device (HID %s) found. Systems "
			"with an RTC on an OS-managed bus MUST implement a "
			"TAD with functioning _GCP (bit 2 set), _GRT and "
			"_SRT. Re-run with --brs-i-no-osbus-rtc if this "
			"system has no such RTC.",
			HID_TAD);
	else if (ctx.failed)
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_060",
			"%u of %u Time and Alarm Device(s) failed _GCP bit 2, "
			"_GRT or _SRT.",
			ctx.failed, ctx.found);
	else
		fwts_passed(fw,
			"AML_060: %u Time and Alarm Device(s) implement "
			"functioning _GCP (bit 2 set), _GRT and _SRT.",
			ctx.found);

	return FWTS_OK;
}

static int method_brsi_aml070(fwts_framework *fw)
{
	method_brsi_tad_ctx ctx;

	/*
	 * AML_070: a TAD must work with no vendor OS driver loaded.
	 * fwts evaluates AML with its own ACPICA instance, which does not
	 * use kernel I2C/SPI/UART drivers or GenericSerialBus handlers
	 * installed in the in-kernel interpreter.
	 */
	fwts_log_info(fw,
		"AML_070: evaluate TAD _GCP/_GRT/_SRT with fwts ACPICA. "
		"This interpreter does not use kernel-loaded I2C/SPI/UART "
		"drivers or in-kernel GenericSerialBus handlers.");
	fwts_log_info(fw,
		"AML_070: SystemMemory/SystemIO OperationRegions can work "
		"here (same physical address as the kernel). GenericSerialBus "
		"Field accesses usually fail in fwts even if a mainline bus "
		"driver is bound and the same AML works in the kernel.");
	fwts_log_info(fw,
		"AML_070: a pass is stricter than \"works after the OS loads "
		"a bus driver\". The driver-loaded OperationRegion switch "
		"is not tested.");

	memset(&ctx, 0, sizeof(ctx));
	ctx.fw = fw;
	ctx.id = "AML_070";

	AcpiGetDevices(HID_TAD, method_brsi_tad_walk, &ctx, NULL);

	if (ctx.found == 0) {
		fwts_skipped(fw,
			"AML_070: no Time and Alarm Device (HID %s) found; "
			"requirement applies only when a TAD is implemented.",
			HID_TAD);
		return FWTS_OK;
	}

	if (ctx.failed) {
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_070",
			"%u of %u Time and Alarm Device(s) failed _GCP, _GRT "
			"or _SRT under fwts ACPICA.",
			ctx.failed, ctx.found);
		fwts_advice(fw,
			"AML_070 requires a TAD to work without extra "
			"system-specific OS drivers. fwts uses a private "
			"ACPICA instance: kernel I2C/SPI/UART drivers do "
			"not install GenericSerialBus handlers for this "
			"evaluator. A failure often means _GRT/_SRT only "
			"work through a driver-backed GenericSerialBus "
			"OperationRegion. Confirm whether a SystemMemory "
			"fallback exists for the no-driver path. A kernel "
			"success with drivers loaded does not satisfy this "
			"fwts check. The AML switch onto a driver-backed "
			"region after the driver loads is not tested.");
	} else {
		fwts_passed(fw,
			"AML_070: %u Time and Alarm Device(s) are functional "
			"under fwts ACPICA without kernel bus drivers.",
			ctx.found);
		fwts_advice(fw,
			"This pass means _GCP/_GRT/_SRT ran in fwts ACPICA "
			"with no kernel GenericSerialBus handler, which is "
			"stricter than \"the TAD works once a mainline I2C/"
			"SPI/UART driver is bound\". It does not verify that "
			"AML later switches to a driver-backed OperationRegion "
			"when that driver is loaded.");
	}

	return FWTS_OK;
}

typedef struct {
	fwts_framework *fw;
	const char *hid;
	const char *kind;
	unsigned int found;
	unsigned int failed;
} method_brsi_gsb_ctx;

static ACPI_STATUS method_brsi_gsb_walk(
	ACPI_HANDLE handle,
	UINT32 nesting_level,
	void *context,
	void **return_value)
{
	method_brsi_gsb_ctx *ctx = context;
	char device_path[128];
	uint64_t gsb = 0;

	FWTS_UNUSED(nesting_level);
	FWTS_UNUSED(return_value);

	ctx->found++;

	method_brsi_acpi_fullname(handle, device_path, sizeof(device_path));

	fwts_log_info(ctx->fw, "AML_080: found %s %s (HID %s).",
		ctx->kind, device_path, ctx->hid);

	if (!method_brsi_eval(handle, "_GSB", NULL, 0, &gsb)) {
		fwts_log_info(ctx->fw,
			"AML_080: %s._GSB is mandatory but missing, failed "
			"to evaluate, or did not return an Integer.",
			device_path);
		ctx->failed++;
	} else {
		fwts_log_info(ctx->fw,
			"AML_080: %s._GSB returned GSI base 0x%" PRIx64 ".",
			device_path, gsb);
	}

	return AE_OK;
}

static int method_brsi_aml080(fwts_framework *fw)
{
	method_brsi_gsb_ctx ctx;

	/*
	 * AML_080: every PLIC (RSCV0001) and APLIC (RSCV0002) namespace
	 * device must implement _GSB returning the GSI base as an Integer.
	 * Device presence when MADT has matching entries is AML_100.
	 */
	memset(&ctx, 0, sizeof(ctx));
	ctx.fw = fw;

	ctx.hid = HID_PLIC;
	ctx.kind = "PLIC";
	AcpiGetDevices(HID_PLIC, method_brsi_gsb_walk, &ctx, NULL);

	ctx.hid = HID_APLIC;
	ctx.kind = "APLIC";
	AcpiGetDevices(HID_APLIC, method_brsi_gsb_walk, &ctx, NULL);

	if (ctx.found == 0) {
		fwts_skipped(fw,
			"AML_080: no PLIC (HID %s) or APLIC (HID %s) device "
			"found; _GSB is required on those objects when they "
			"exist.",
			HID_PLIC, HID_APLIC);
		return FWTS_OK;
	}

	if (ctx.failed)
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_080",
			"%u of %u PLIC/APLIC device(s) failed _GSB.",
			ctx.failed, ctx.found);
	else
		fwts_passed(fw,
			"AML_080: %u PLIC/APLIC device(s) implement _GSB "
			"returning an Integer GSI base.",
			ctx.found);

	return FWTS_OK;
}

typedef struct {
	fwts_framework *fw;
	unsigned int found;
	unsigned int failed;
	bool clock_ok;
} method_brsi_uart_ctx;

static bool method_brsi_dsd_uuid_match(const ACPI_OBJECT *obj)
{
	if (obj == NULL || obj->Type != ACPI_TYPE_BUFFER)
		return false;
	if (obj->Buffer.Length != sizeof(dsd_devprop_uuid))
		return false;
	return memcmp(obj->Buffer.Pointer, dsd_devprop_uuid,
		sizeof(dsd_devprop_uuid)) == 0;
}

static void method_brsi_uart_dsd_return(
	fwts_framework *fw,
	char *name,
	ACPI_BUFFER *buf,
	ACPI_OBJECT *obj,
	void *private)
{
	method_brsi_uart_ctx *ctx = private;
	uint32_t i, j;

	FWTS_UNUSED(buf);

	if (obj == NULL || obj->Type != ACPI_TYPE_PACKAGE) {
		fwts_log_info(fw, "AML_090: %s did not return a Package.", name);
		return;
	}

	if (obj->Package.Count & 1) {
		fwts_log_info(fw,
			"AML_090: %s must contain UUID/data pairs "
			"(even element count), got %" PRIu32 ".",
			name, obj->Package.Count);
		return;
	}

	for (i = 0; i < obj->Package.Count; i += 2) {
		ACPI_OBJECT *uuid = &obj->Package.Elements[i];
		ACPI_OBJECT *data = &obj->Package.Elements[i + 1];

		if (!method_brsi_dsd_uuid_match(uuid))
			continue;
		if (data->Type != ACPI_TYPE_PACKAGE) {
			fwts_log_info(fw,
				"AML_090: %s Device Properties data is not "
				"a Package.",
				name);
			return;
		}

		for (j = 0; j < data->Package.Count; j++) {
			ACPI_OBJECT *prop = &data->Package.Elements[j];
			ACPI_OBJECT *key, *val;
			const char *keystr;
			uint64_t valint;

			if (prop->Type != ACPI_TYPE_PACKAGE ||
			    prop->Package.Count < 2) {
				fwts_log_info(fw,
					"AML_090: %s property %" PRIu32
					" is not a 2-element Package.",
					name, j);
				continue;
			}

			key = &prop->Package.Elements[0];
			val = &prop->Package.Elements[1];

			if (key->Type != ACPI_TYPE_STRING) {
				fwts_log_info(fw,
					"AML_090: %s property %" PRIu32
					" name is not a String.",
					name, j);
				continue;
			}
			keystr = key->String.Pointer;

			if (val->Type != ACPI_TYPE_INTEGER) {
				fwts_log_info(fw,
					"AML_090: %s property \"%s\" is not "
					"an Integer.",
					name, keystr);
				continue;
			}
			valint = val->Integer.Value;

			fwts_log_info(fw,
				"AML_090: %s \"%s\" = %" PRIu64 ".",
				name, keystr, valint);

			if (strcmp(keystr, "clock-frequency") == 0) {
				if (valint != 0)
					ctx->clock_ok = true;
				else
					fwts_log_info(fw,
						"AML_090: %s clock-frequency "
						"is 0; baud rate cannot be set.",
						name);
			}

			if (strcmp(keystr, "reg-io-width") == 0) {
				if (valint != 1 && valint != 2 &&
						valint != 4 && valint != 8)
					fwts_log_info(fw,
						"AML_090: %s reg-io-width "
						"must be 1, 2, 4 or 8.",
						name);
			}
		}
		return;
	}

	fwts_log_info(fw,
		"AML_090: %s has no Device Properties UUID "
		"(daffd814-6eba-4d8c-8a91-bc9bbf4aa301).",
		name);
}

static ACPI_STATUS method_brsi_uart_walk(
	ACPI_HANDLE handle,
	UINT32 nesting_level,
	void *context,
	void **return_value)
{
	method_brsi_uart_ctx *ctx = context;
	char device_path[128];

	FWTS_UNUSED(nesting_level);
	FWTS_UNUSED(return_value);

	ctx->found++;
	ctx->clock_ok = false;

	method_brsi_acpi_fullname(handle, device_path, sizeof(device_path));

	fwts_log_info(ctx->fw, "AML_090: found UART %s (HID %s).",
		device_path, HID_UART);

	if (fwts_evaluate_method(ctx->fw, METHOD_MANDATORY | METHOD_SILENT,
			&handle, "_DSD", NULL, 0,
			method_brsi_uart_dsd_return, ctx) != FWTS_OK)
		fwts_log_info(ctx->fw,
			"AML_090: %s._DSD is mandatory but missing or failed "
			"to evaluate.",
			device_path);

	if (!ctx->clock_ok)
		ctx->failed++;

	return AE_OK;
}

static int method_brsi_aml090(fwts_framework *fw)
{
	method_brsi_uart_ctx ctx;

	/*
	 * AML_090: RSCV0003 UART devices must implement the UART device
	 * properties (BRS acpi-prop.adoc) via _DSD Device Properties UUID.
	 * clock-frequency is required and must be a non-zero Integer.
	 */
	memset(&ctx, 0, sizeof(ctx));
	ctx.fw = fw;

	AcpiGetDevices(HID_UART, method_brsi_uart_walk, &ctx, NULL);

	if (ctx.found == 0) {
		fwts_skipped(fw,
			"AML_090: no UART device (HID %s) found; UART "
			"properties are required on those objects when they "
			"exist.",
			HID_UART);
		return FWTS_OK;
	}

	if (ctx.failed)
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_090",
			"%u of %u UART device(s) (HID %s) missing a non-zero "
			"clock-frequency in _DSD Device Properties.",
			ctx.failed, ctx.found, HID_UART);
	else
		fwts_passed(fw,
			"AML_090: %u UART device(s) (HID %s) implement "
			"clock-frequency in _DSD Device Properties.",
			ctx.found, HID_UART);

	return FWTS_OK;
}

static int options_handler(
	fwts_framework *fw,
	int argc,
	char * const argv[],
	int option_char,
	int long_index)
{
	FWTS_UNUSED(argc);
	FWTS_UNUSED(argv);

	if (option_char == 0) {
		switch (long_index) {
		case 0:	/* --brs-i-no-osbus-rtc */
			no_osbus_rtc = true;
			fwts_log_info(fw,
				"BRS-I: no RTC on OS-managed bus, skip AML_060 check");
			break;
		}
	}
	return FWTS_OK;
}

static fwts_option options[] = {
	{ "brs-i-no-osbus-rtc", "", 0,
	  "Platform has no RTC on an OS-managed bus (skip AML_060)" },
	{ NULL, NULL, 0, NULL }
};

static fwts_framework_minor_test method_brsi_tests[] = {
	{ method_brsi_aml010,
	  "AML_010: PCIe Root Complex _CRS SHOULD NOT return I/O ranges." },
	{ method_brsi_aml020,
	  "AML_020: _PRS and _SRS methods SHOULD NOT be implemented." },
	{ method_brsi_aml030,
	  "AML_030: per-hart devices MUST be under \\_SB, not \\_PR." },
	{ method_brsi_aml060,
	  "AML_060: TAD with _GCP bit 2, _GRT and _SRT if RTC is on an OS-managed bus." },
	{ method_brsi_aml070,
	  "AML_070: TAD MUST work in fwts ACPICA without kernel bus drivers." },
	{ method_brsi_aml080,
	  "AML_080: PLIC and APLIC devices MUST implement _GSB." },
	{ method_brsi_aml090,
	  "AML_090: RSCV0003 UART devices MUST implement UART device properties." },
	{ NULL, NULL }
};

static fwts_framework_ops method_brsi_ops = {
	.description = "RISC-V BRS-I ACPI Methods and Objects test.",
	.init        = method_brsi_init,
	.deinit      = method_brsi_deinit,
	.minor_tests = method_brsi_tests,
	.options     = options,
	.options_handler = options_handler
};

FWTS_REGISTER("method_brsi", &method_brsi_ops, FWTS_TEST_ANYTIME, FWTS_FLAG_BRSI)

#endif
