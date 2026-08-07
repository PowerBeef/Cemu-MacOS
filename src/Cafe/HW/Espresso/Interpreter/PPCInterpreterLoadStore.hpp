
#define _signExtend16To32(__v) ((uint32)(sint32)(sint16)(__v))

// store

#define DSI_EXIT() \
	if constexpr(ppcItpCtrl::allowDSI) \
	{ \
		if (hCPU->memoryException) \
		{ \
			hCPU->memoryException = false; \
			return; \
		} \
	}

static void PPCInterpreter_STW(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rS, rA, imm);
	if (rA != 0)
	{
		ppcItpCtrl::ppcMem_writeDataU32(hCPU, hCPU->gpr[rA] + imm, hCPU->gpr[rS]);
	}
	else
	{
		PPC_ASSERT(true);
	}
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STWU(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rS, rA, imm);
	ppcItpCtrl::ppcMem_writeDataU32(hCPU, hCPU->gpr[rA] + imm, hCPU->gpr[rS]);
	// check for rA != 0 ? 
	hCPU->gpr[rA] += imm;
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STWX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS, rB;
	PPC_OPC_TEMPL_X(Opcode, rS, rA, rB);
	ppcItpCtrl::ppcMem_writeDataU32(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB], hCPU->gpr[rS]);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STWCX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	// http://www.ibm.com/developerworks/library/pa-atom/
	// Espresso (ppc750cl.s): stwcx. may target a *different* EA than lwarx and
	// still succeed if the reserved location's value is unchanged; the store
	// goes to the stwcx EA (suite writes r4 while reserved was r4+4).
	sint32 rA, rS, rB;
	PPC_OPC_TEMPL_X(Opcode, rS, rA, rB);
	const uint32 ea = (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB];

	if (hCPU->hasMemReservation)
	{
		const uint32 resAddr = hCPU->reservedMemAddr;
		const uint32be expected = hCPU->reservedMemValue;
		std::atomic<uint32be>* reservedPtr;
		if constexpr (ppcItpCtrl::allowSupervisorMode)
		{
			reservedPtr = _rawPtrToAtomic((uint32be*)(memory_base + ppcItpCtrl::ppcMem_translateVirtualDataToPhysicalAddr(hCPU, resAddr)));
			DSI_EXIT();
		}
		else
		{
			reservedPtr = _rawPtrToAtomic((uint32be*)memory_getPointerFromVirtualOffset(resAddr));
		}
		if (reservedPtr->load() == expected)
		{
			// Store to the stwcx. EA (may differ from resAddr).
			std::atomic<uint32be>* storePtr;
			if constexpr (ppcItpCtrl::allowSupervisorMode)
			{
				storePtr = _rawPtrToAtomic((uint32be*)(memory_base + ppcItpCtrl::ppcMem_translateVirtualDataToPhysicalAddr(hCPU, ea)));
				DSI_EXIT();
			}
			else
			{
				storePtr = _rawPtrToAtomic((uint32be*)memory_getPointerFromVirtualOffset(ea));
			}
			storePtr->store((uint32be)hCPU->gpr[rS]);
			ppc_setCRBit(hCPU, CR_BIT_LT, 0);
			ppc_setCRBit(hCPU, CR_BIT_GT, 0);
			ppc_setCRBit(hCPU, CR_BIT_EQ, 1);
		}
		else
		{
			ppc_setCRBit(hCPU, CR_BIT_LT, 0);
			ppc_setCRBit(hCPU, CR_BIT_GT, 0);
			ppc_setCRBit(hCPU, CR_BIT_EQ, 0);
		}
		cemu_assert_debug(hCPU->xer_so <= 1);
		ppc_setCRBit(hCPU, CR_BIT_SO, hCPU->xer_so);
		hCPU->hasMemReservation = 0;
		hCPU->reservedMemAddr = 0;
		hCPU->reservedMemValue = 0;
	}
	else
	{
		ppc_setCRBit(hCPU, CR_BIT_LT, 0);
		ppc_setCRBit(hCPU, CR_BIT_GT, 0);
		ppc_setCRBit(hCPU, CR_BIT_EQ, 0);
		cemu_assert_debug(hCPU->xer_so <= 1);
		ppc_setCRBit(hCPU, CR_BIT_SO, hCPU->xer_so);
	}
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STWUX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS, rB;
	PPC_OPC_TEMPL_X(Opcode, rS, rA, rB);
	ppcItpCtrl::ppcMem_writeDataU32(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB], hCPU->gpr[rS]);
	if (rA)
		hCPU->gpr[rA] += hCPU->gpr[rB];
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STWBRX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS, rB;
	PPC_OPC_TEMPL_X(Opcode, rS, rA, rB);
	ppcItpCtrl::ppcMem_writeDataU32(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB], _swapEndianU32(hCPU->gpr[rS]));
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STMW(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rS, rA;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rS, rA, imm);
	uint32 ea = (rA ? hCPU->gpr[rA] : 0) + imm;
	while (rS <= 31)
	{
		ppcItpCtrl::ppcMem_writeDataU32(hCPU, ea, hCPU->gpr[rS]);
		rS++;
		ea += 4;
	}
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STH(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rS, rA, imm);
	ppcItpCtrl::ppcMem_writeDataU16(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm, (uint16)hCPU->gpr[rS]);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STHU(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rS, rA, imm);
	ppcItpCtrl::ppcMem_writeDataU16(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm, (uint16)hCPU->gpr[rS]);
	if (rA)
		hCPU->gpr[rA] += imm;
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STHX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS, rB;
	PPC_OPC_TEMPL_X(Opcode, rS, rA, rB);
	ppcItpCtrl::ppcMem_writeDataU16(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB], (uint16)hCPU->gpr[rS]);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STHUX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS, rB;
	PPC_OPC_TEMPL_X(Opcode, rS, rA, rB);
	ppcItpCtrl::ppcMem_writeDataU16(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB], (uint16)hCPU->gpr[rS]);
	if (rA)
		hCPU->gpr[rA] += hCPU->gpr[rB];
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STHBRX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS, rB;
	PPC_OPC_TEMPL_X(Opcode, rS, rA, rB);
	ppcItpCtrl::ppcMem_writeDataU16(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB], _swapEndianU16((uint16)hCPU->gpr[rS]));
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STB(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rS, rA, imm);
	ppcItpCtrl::ppcMem_writeDataU8(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm, (uint8)hCPU->gpr[rS]);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STBU(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rS, rA, imm);
	ppcItpCtrl::ppcMem_writeDataU8(hCPU, hCPU->gpr[rA] + imm, (uint8)hCPU->gpr[rS]);
	hCPU->gpr[rA] += imm;
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STBX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS, rB;
	PPC_OPC_TEMPL_X(Opcode, rS, rA, rB);
	ppcItpCtrl::ppcMem_writeDataU8(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB], (uint8)hCPU->gpr[rS]);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STBUX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS, rB;
	PPC_OPC_TEMPL_X(Opcode, rS, rA, rB);
	ppcItpCtrl::ppcMem_writeDataU8(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB], (uint8)hCPU->gpr[rS]);
	if (rA)
		hCPU->gpr[rA] += hCPU->gpr[rB];
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STSWI(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS, nb;
	PPC_OPC_TEMPL_X(Opcode, rS, rA, nb);
	if (nb == 0) nb = 32;
	uint32 ea = rA ? hCPU->gpr[rA] : 0;
	uint32 r = 0;
	int i = 0;
	while (nb > 0)
	{
		if (i == 0)
		{
			r = rS < 32 ? hCPU->gpr[rS] : 0; // what happens if rS is out of bounds?
			rS++;
			rS %= 32;
			i = 4;
		}
		ppcItpCtrl::ppcMem_writeDataU8(hCPU, ea, (r >> 24));
		r <<= 8;
		ea++;
		i--;
		nb--;
	}
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STSWX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS, rB;
	PPC_OPC_TEMPL_X(Opcode, rS, rA, rB);
	sint32 nb = hCPU->spr.XER&0x7F;
	if (nb == 0)
	{
		PPCInterpreter_nextInstruction(hCPU);
		return;
	}
	uint32 ea = rA ? hCPU->gpr[rA] : 0;
	ea += hCPU->gpr[rB];
	uint32 r = 0;
	int i = 0;
	while (nb > 0)
	{
		if (i == 0)
		{
			r = rS < 32 ? hCPU->gpr[rS] : 0; // what happens if rS is out of bounds?
			rS++;
			rS %= 32;
			i = 4;
		}
		ppcItpCtrl::ppcMem_writeDataU8(hCPU, ea, (r >> 24));
		r <<= 8;
		ea++;
		i--;
		nb--;
	}
	PPCInterpreter_nextInstruction(hCPU);
}

// load

static void PPCInterpreter_LWZ(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rD, rA, imm);
	uint32 v = ppcItpCtrl::ppcMem_readDataU32(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm);
	DSI_EXIT();
	hCPU->gpr[rD] = v;
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LWZU(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rD, rA, imm);
	hCPU->gpr[rA] += imm;
	hCPU->gpr[rD] = ppcItpCtrl::ppcMem_readDataU32(hCPU, hCPU->gpr[rA]);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LMW(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rD, rA;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rD, rA, imm);
	uint32 ea = (rA ? hCPU->gpr[rA] : 0) + imm;
	while (rD <= 31)
	{
		hCPU->gpr[rD] = ppcItpCtrl::ppcMem_readDataU32(hCPU, ea);
		rD++;
		ea += 4;
	}
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LWZX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD, rB;
	PPC_OPC_TEMPL_X(Opcode, rD, rA, rB);
	hCPU->gpr[rD] = ppcItpCtrl::ppcMem_readDataU32(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB]);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LWZXU(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD, rB;
	PPC_OPC_TEMPL_X(Opcode, rD, rA, rB);
	uint32 ea = (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB];
	hCPU->gpr[rD] = ppcItpCtrl::ppcMem_readDataU32(hCPU, ea);
	if (rA && rA != rD)
		hCPU->gpr[rA] = ea;
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LWBRX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD, rB;
	PPC_OPC_TEMPL_X(Opcode, rD, rA, rB);
	hCPU->gpr[rD] = CPU_swapEndianU32(ppcItpCtrl::ppcMem_readDataU32(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB]));

	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LWARX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD, rB;
	PPC_OPC_TEMPL_X(Opcode, rD, rA, rB);
	uint32 ea = (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB];
	hCPU->gpr[rD] = ppcItpCtrl::ppcMem_readDataU32(hCPU, ea);
	// set reservation	
	hCPU->reservedMemAddr = ea;
	hCPU->reservedMemValue = hCPU->gpr[rD];
	hCPU->hasMemReservation = 1;
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LHZ(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rD, rA, imm);
	hCPU->gpr[rD] = ppcItpCtrl::ppcMem_readDataU16(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LHZU(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rD, rA, imm);
	// FIXME: rA!=0
	hCPU->gpr[rD] = ppcItpCtrl::ppcMem_readDataU16(hCPU, hCPU->gpr[rA] + imm);
	hCPU->gpr[rA] += imm;
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LHZX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD, rB;
	PPC_OPC_TEMPL_X(Opcode, rD, rA, rB);
	hCPU->gpr[rD] = ppcItpCtrl::ppcMem_readDataU16(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB]);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LHZUX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD, rB;
	PPC_OPC_TEMPL_X(Opcode, rD, rA, rB);
	uint32 ea = (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB];
	hCPU->gpr[rD] = ppcItpCtrl::ppcMem_readDataU16(hCPU, ea);
	if (rA && rA != rD)
		hCPU->gpr[rA] = ea;
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LHBRX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD, rB;
	PPC_OPC_TEMPL_X(Opcode, rD, rA, rB);
	hCPU->gpr[rD] = CPU_swapEndianU16(ppcItpCtrl::ppcMem_readDataU16(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB]));
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LHA(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rD, rA, imm);
	hCPU->gpr[rD] = ppcItpCtrl::ppcMem_readDataU16(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm);
	hCPU->gpr[rD] = _signExtend16To32(hCPU->gpr[rD]);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LHAU(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rD, rA, imm);
	hCPU->gpr[rD] = ppcItpCtrl::ppcMem_readDataU16(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm);
	if (rA && rA != rD)
		hCPU->gpr[rA] += imm;
	hCPU->gpr[rD] = _signExtend16To32(hCPU->gpr[rD]);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LHAUX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD, rB;
	PPC_OPC_TEMPL_X(Opcode, rD, rA, rB);
	uint32 ea = (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB];
	hCPU->gpr[rD] = ppcItpCtrl::ppcMem_readDataU16(hCPU, ea);
	if (rA && rA != rD)
		hCPU->gpr[rA] = ea;
	hCPU->gpr[rD] = _signExtend16To32(hCPU->gpr[rD]);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LHAX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rS, rB;
	PPC_OPC_TEMPL_X(Opcode, rS, rA, rB);

	hCPU->gpr[rS] = ppcItpCtrl::ppcMem_readDataU16(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB]);
	hCPU->gpr[rS] = _signExtend16To32(hCPU->gpr[rS]);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LBZ(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rD, rA, imm);
	hCPU->gpr[rD] = ppcItpCtrl::ppcMem_readDataU8(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LBZX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD, rB;
	PPC_OPC_TEMPL_X(Opcode, rD, rA, rB);
	hCPU->gpr[rD] = ppcItpCtrl::ppcMem_readDataU8(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB]);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LBZXU(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD, rB;
	PPC_OPC_TEMPL_X(Opcode, rD, rA, rB);
	uint32 ea = (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB];
	hCPU->gpr[rD] = ppcItpCtrl::ppcMem_readDataU8(hCPU, ea);
	if (rA && rA != rD)
		hCPU->gpr[rA] = ea;
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LBZU(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, rD, rA, imm);
	PPC_ASSERT(rA == 0);
	uint8 r;
	uint32 ea = hCPU->gpr[rA] + imm;
	hCPU->gpr[rA] = ea;
	r = ppcItpCtrl::ppcMem_readDataU8(hCPU, ea);
	hCPU->gpr[rD] = r;
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LSWI(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD, nb;
	PPC_OPC_TEMPL_X(Opcode, rD, rA, nb);
	if (nb == 0)
		nb = 32;
	uint32 ea = rA ? hCPU->gpr[rA] : 0;
	uint32 r = 0;
	int i = 4;
	uint8 v;
	while (nb>0)
	{
		if (i == 0)
		{
			i = 4;
			if(rD < 32)
				hCPU->gpr[rD] = r;
			rD++;
			rD %= 32;
			r = 0;
		}
		v = ppcItpCtrl::ppcMem_readDataU8(hCPU, ea);
		r <<= 8;
		r |= v;
		ea++;
		i--;
		nb--;
	}
	while (i)
	{
		r <<= 8;
		i--;
	}
	if(rD < 32)
		hCPU->gpr[rD] = r;
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LSWX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	sint32 rA, rD, rB;
	PPC_OPC_TEMPL_X(Opcode, rD, rA, rB);
	// byte count comes from XER
	uint32 nb = (hCPU->spr.XER>>0)&0x7F;
	if (nb == 0)
	{
		PPCInterpreter_nextInstruction(hCPU);
		return; // no-op
	}
	uint32 ea = rA ? hCPU->gpr[rA] : 0;
	ea += hCPU->gpr[rB];
	uint32 r = 0;
	int i = 4;
	uint8 v;
	while (nb>0)
	{
		if (i == 0)
		{
			i = 4;
			if(rD < 32)
				hCPU->gpr[rD] = r;
			rD++;
			rD %= 32;
			r = 0;
		}
		v = ppcItpCtrl::ppcMem_readDataU8(hCPU, ea);
		r <<= 8;
		r |= v;
		ea++;
		i--;
		nb--;
	}
	while (i)
	{
		r <<= 8;
		i--;
	}
	if(rD < 32)
		hCPU->gpr[rD] = r;
	PPCInterpreter_nextInstruction(hCPU);
}

// floating point load

static void PPCInterpreter_LFS(PPCInterpreter_t* hCPU, uint32 Opcode) //Copied
{
	FPUCheckAvailable();
	sint32 rA, frD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, frD, rA, imm);

	uint64 val;
	//*(uint32*)&Val = ppcItpCtrl::ppcMem_readDataU32(hCPU, (rA?hCPU->gpr[rA]:0)+imm);
	val = ppcItpCtrl::ppcMem_readDataFloatEx(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm);

	if (PPC_LSQE)
		hCPU->fpr[frD].fp0int = hCPU->fpr[frD].fp1int = val;
	else
		hCPU->fpr[frD].fp0int = val;

	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LFSX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	sint32 rA, frD, rB;
	PPC_OPC_TEMPL_X(Opcode, frD, rA, rB);

	uint64 val;
	val = ppcItpCtrl::ppcMem_readDataFloatEx(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB]);

	if (PPC_LSQE)
		hCPU->fpr[frD].fp0int = hCPU->fpr[frD].fp1int = val;
	else
		hCPU->fpr[frD].fp0int = val;
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LFSUX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	sint32 rA, frD, rB;
	PPC_OPC_TEMPL_X(Opcode, frD, rA, rB);

	uint64 Val;
	//*(uint32*)&Val = ppcItpCtrl::ppcMem_readDataU32(hCPU, (rA?hCPU->gpr[rA]:0)+hCPU->gpr[rB]);
	Val = ppcItpCtrl::ppcMem_readDataFloatEx(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB]);
	if (rA)
		hCPU->gpr[rA] += hCPU->gpr[rB];

	if (PPC_LSQE)
		hCPU->fpr[frD].fp0int = hCPU->fpr[frD].fp1int = Val;
	else
		hCPU->fpr[frD].fp0int = Val;//ppcItpCtrl::ppcMem_readDataFloat((rA?hCPU->gpr[rA]:0)+hCPU->gpr[rB]);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LFSU(PPCInterpreter_t* hCPU, uint32 Opcode) //Copied
{
	FPUCheckAvailable();
	sint32 rA, frD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, frD, rA, imm);
	uint64 Val;

	//(uint32*)&Val = ppcItpCtrl::ppcMem_readDataU32(hCPU, (rA?hCPU->gpr[rA]:0)+imm);
	Val = ppcItpCtrl::ppcMem_readDataFloatEx(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm);


	if (PPC_LSQE)
		hCPU->fpr[frD].fp0int = hCPU->fpr[frD].fp1int = Val;
	else
		hCPU->fpr[frD].fp0int = Val;

	if (rA)
		hCPU->gpr[rA] += imm;

	PPCInterpreter_nextInstruction(hCPU);
}

// After lfd under PSE: incomplete interlocking can leak the loaded double's
// high word into the ps1 shadow (suite). Observed when a prior PS write left
// the shadow at ±0 (ps_mr of zero); isync clears dirty so lfd_ps is safe.
// Non-zero shadows after merge/etc. are preserved (suite fmr-over-PS tests).
static inline void PPCInterpreter_LFD_FinishPs1(PPCInterpreter_t* hCPU, sint32 frD, double v)
{
	if (!PPC_PSE)
		return;
	const uint32 bit = 1u << (frD & 31);
	if ((hCPU->psWriteDirty & bit) == 0)
		return;
	hCPU->psWriteDirty &= ~bit;
	uint64 ps1bits;
	std::memcpy(&ps1bits, &hCPU->fpr[frD].fp1, sizeof(ps1bits));
	if ((ps1bits & 0x7FFFFFFFFFFFFFFFULL) != 0)
		return; // non-zero shadow kept
	hCPU->fpr[frD].fp1 = ppc_lfd_ps_shadow(v);
}

// Mark dest FPR as having received a paired-single write (for lfd hazard).
static inline void PPCInterpreter_NotePsWrite(PPCInterpreter_t* hCPU, sint32 frD)
{
	if (PPC_PSE)
		hCPU->psWriteDirty |= 1u << (frD & 31);
}

static void PPCInterpreter_LFD(PPCInterpreter_t* hCPU, uint32 Opcode) //Copied
{
	FPUCheckAvailable();
	sint32 rA, frD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, frD, rA, imm);
	const double v = ppcItpCtrl::ppcMem_readDataDouble(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm);
	hCPU->fpr[frD].fpr = v;
	PPCInterpreter_LFD_FinishPs1(hCPU, frD, v);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LFDU(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	sint32 rA, frD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, frD, rA, imm);

	const double v = ppcItpCtrl::ppcMem_readDataDouble(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm);
	hCPU->fpr[frD].fpr = v;
	PPCInterpreter_LFD_FinishPs1(hCPU, frD, v);
	if (rA)
		hCPU->gpr[rA] += imm;
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LFDX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	sint32 rA, frD, rB;
	PPC_OPC_TEMPL_X(Opcode, frD, rA, rB);
	const double v = ppcItpCtrl::ppcMem_readDataDouble(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB]);
	hCPU->fpr[frD].fpr = v;
	PPCInterpreter_LFD_FinishPs1(hCPU, frD, v);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_LFDUX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	sint32 rA, frD, rB;
	PPC_OPC_TEMPL_X(Opcode, frD, rA, rB);
	const double v = ppcItpCtrl::ppcMem_readDataDouble(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB]);
	hCPU->fpr[frD].fpr = v;
	PPCInterpreter_LFD_FinishPs1(hCPU, frD, v);
	if (rA)
		hCPU->gpr[rA] += hCPU->gpr[rB];
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STFS(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	sint32 rA, frD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, frD, rA, imm);
	if (PPC_LSQE)
		ppcItpCtrl::ppcMem_writeDataFloatEx(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm, hCPU->fpr[frD].fp0int);
	else
		ppcItpCtrl::ppcMem_writeDataFloatEx(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm, hCPU->fpr[frD].fp0int);
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STFSU(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	sint32 rA, frD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, frD, rA, imm);

	if (PPC_LSQE)
		ppcItpCtrl::ppcMem_writeDataFloatEx(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm, hCPU->fpr[frD].fp0int);
	else
		ppcItpCtrl::ppcMem_writeDataFloatEx(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm, hCPU->fpr[frD].fp0int);

	if (rA)
		hCPU->gpr[rA] += imm;
	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STFSX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	sint32 rA, frS, rB;
	PPC_OPC_TEMPL_X(Opcode, frS, rA, rB);

	if (PPC_LSQE)
		ppcItpCtrl::ppcMem_writeDataFloatEx(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB], hCPU->fpr[frS].fp0int);
	else
		ppcItpCtrl::ppcMem_writeDataFloatEx(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB], hCPU->fpr[frS].fp0int);

	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_STFSUX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	// next instruction
	PPCInterpreter_nextInstruction(hCPU);

	int rA, frS, rB;
	PPC_OPC_TEMPL_X(Opcode, frS, rA, rB);

	if (PPC_LSQE)
		ppcItpCtrl::ppcMem_writeDataFloatEx(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB], hCPU->fpr[frS].fp0int);
	else
		ppcItpCtrl::ppcMem_writeDataFloatEx(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB], hCPU->fpr[frS].fp0int);

	if (rA)
		hCPU->gpr[rA] += hCPU->gpr[rB];
}


static void PPCInterpreter_STFD(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	// next instruction
	PPCInterpreter_nextInstruction(hCPU);

	int rA, frD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, frD, rA, imm);

	ppcItpCtrl::ppcMem_writeDataDouble(hCPU, (rA ? hCPU->gpr[rA] : 0) + imm, hCPU->fpr[frD].fpr);

	// debug output
#ifdef __DEBUG_OUTPUT_INSTRUCTION
	debug_printf("STFD f%d, %d(r%d)\n", frD, imm, rA);
#endif
}

static void PPCInterpreter_STFDU(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	// next instruction
	PPCInterpreter_nextInstruction(hCPU);

	int rA, frD;
	uint32 imm;
	PPC_OPC_TEMPL_D_SImm(Opcode, frD, rA, imm);

	if (rA)
	{
		hCPU->gpr[rA] += imm;
	}
	else
	{
		PPC_ASSERT(true);
	}

	ppcItpCtrl::ppcMem_writeDataDouble(hCPU, hCPU->gpr[rA], hCPU->fpr[frD].fpr);

	// debug output
#ifdef __DEBUG_OUTPUT_INSTRUCTION
	debug_printf("STFD f%d, %d(r%d)\n", frD, imm, rA);
#endif
}

static void PPCInterpreter_STFDX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	// next instruction
	PPCInterpreter_nextInstruction(hCPU);

	int rA, frS, rB;
	PPC_OPC_TEMPL_X(Opcode, frS, rA, rB);

	ppcItpCtrl::ppcMem_writeDataDouble(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB], hCPU->fpr[frS].fpr);

	// debug output
#ifdef __DEBUG_OUTPUT_INSTRUCTION
	debug_printf("STFD f%d, r%d+r%d\n", frS, rA, rB);
#endif
}

static void PPCInterpreter_STFDUX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	// next instruction
	PPCInterpreter_nextInstruction(hCPU);

	int rA, frS, rB;
	PPC_OPC_TEMPL_X(Opcode, frS, rA, rB);

	if (rA == 0)
	{
		ppcItpCtrl::ppcMem_writeDataDouble(hCPU, hCPU->gpr[rB], hCPU->fpr[frS].fpr);
	}
	else
	{
		hCPU->gpr[rA] += hCPU->gpr[rB];
		ppcItpCtrl::ppcMem_writeDataDouble(hCPU, hCPU->gpr[rA], hCPU->fpr[frS].fpr);
	}

}

static void PPCInterpreter_STFIWX(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	FPUCheckAvailable();
	sint32 rA, frS, rB;
	PPC_OPC_TEMPL_X(Opcode, frS, rA, rB);

	uint32 val = (uint32)hCPU->fpr[frS].fp0int;
	ppcItpCtrl::ppcMem_writeDataU32(hCPU, (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB], val);
	// next instruction
	PPCInterpreter_nextInstruction(hCPU);
}

// paired single

// ST_TYPE:
// 4 - uint8
// 5 - uint16
// 6 - sint8
// 7 - sint16
// 0 - float32

#define LD_SCALE(n) ((hCPU->spr.UGQR[0+n] >> 24) & 0x3f)
#define LD_TYPE(n)  ((hCPU->spr.UGQR[0+n] >> 16) & 7)
#define ST_SCALE(n) ((hCPU->spr.UGQR[0+n] >>  8) & 0x3f)
#define ST_TYPE(n)  ((hCPU->spr.UGQR[0+n]      ) & 7)
#define PSW         (opcode & 0x8000)
#define PSI         ((opcode >> 12) & 7)

#define PSWX         (opcode & (1<<(7+3)))
#define PSIX         ((opcode >> 7) & 7)

// PSQ D-form low halfword is NOT a plain 16-bit signed imm: bits 0-11 = d (signed),
// bits 12-14 = I (GQR), bit 15 = W. Using PPC_OPC_TEMPL_D_SImm folds W/I into the
// displacement and breaks every W=1 or nonzero-I form.
static inline sint32 psq_d_form_disp(uint32 opcode)
{
	return (sint32)(opcode << 20) >> 20; // sign-extend 12-bit field in bits 0-11
}

static void psq_store_pair(PPCInterpreter_t* hCPU, uint32 ea, sint32 frD, sint32 type, uint8 scale, bool oneSlot)
{
	// Pass doubles so type-0 store can ConvertToSingleNoFTZ (truncate, preserve NaN).
	if ((type == 4) || (type == 6))
		ppcItpCtrl::ppcMem_writeDataU8(hCPU, ea, quantize(hCPU->fpr[frD].fp0, type, scale));
	else if ((type == 5) || (type == 7))
		ppcItpCtrl::ppcMem_writeDataU16(hCPU, ea, quantize(hCPU->fpr[frD].fp0, type, scale));
	else
		ppcItpCtrl::ppcMem_writeDataU32(hCPU, ea, quantize(hCPU->fpr[frD].fp0, type, scale));

	if (oneSlot)
		return;

	if ((type == 4) || (type == 6))
		ppcItpCtrl::ppcMem_writeDataU8(hCPU, ea + 1, quantize(hCPU->fpr[frD].fp1, type, scale));
	else if ((type == 5) || (type == 7))
		ppcItpCtrl::ppcMem_writeDataU16(hCPU, ea + 2, quantize(hCPU->fpr[frD].fp1, type, scale));
	else
		ppcItpCtrl::ppcMem_writeDataU32(hCPU, ea + 4, quantize(hCPU->fpr[frD].fp1, type, scale));
}

static void PPCInterpreter_PSQ_ST(PPCInterpreter_t* hCPU, unsigned int opcode)
{
	FPUCheckAvailable();
	const sint32 frD = (opcode >> 21) & 0x1F;
	const sint32 rA = (opcode >> 16) & 0x1F;
	const uint32 ea = (uint32)psq_d_form_disp(opcode) + (rA ? hCPU->gpr[rA] : 0);

	const sint32 type = ST_TYPE(PSI);
	const uint8 scale = (uint8)ST_SCALE(PSI);
	psq_store_pair(hCPU, ea, frD, type, scale, (opcode & 0x8000) != 0);

	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_PSQ_STU(PPCInterpreter_t* hCPU, unsigned int opcode)
{
	FPUCheckAvailable();
	const sint32 frD = (opcode >> 21) & 0x1F;
	const sint32 rA = (opcode >> 16) & 0x1F;
	const uint32 ea = (uint32)psq_d_form_disp(opcode) + (rA ? hCPU->gpr[rA] : 0);
	if (rA)
		hCPU->gpr[rA] = ea;

	const sint32 type = ST_TYPE(PSI);
	const uint8 scale = (uint8)ST_SCALE(PSI);
	psq_store_pair(hCPU, ea, frD, type, scale, (opcode & 0x8000) != 0);

	PPCInterpreter_nextInstruction(hCPU);
}


// X-form psq_stx (XO=7) and psq_stux (XO=39). Same 5-bit secondary switch
// case — bit 6 of the instruction word (XO bit 5) selects the update form.
static void PPCInterpreter_PSQ_STX(PPCInterpreter_t* hCPU, unsigned int opcode)
{
	FPUCheckAvailable();

	const sint32 frD = (opcode >> 21) & 0x1F;
	const uint32 rA = (opcode >> 16) & 0x1F;
	const uint32 rB = (opcode >> 11) & 0x1F;
	// X-form EA: rA==0 means zero (not gpr0); rB always uses gpr[rB] (rB==0 is gpr0).
	const uint32 EA = (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB];
	const bool withUpdate = (opcode & 0x40) != 0; // psq_stux
	if (withUpdate && rA)
		hCPU->gpr[rA] = EA;

	const sint32 type = ST_TYPE(PSIX);
	const uint8 scale = (uint8)ST_SCALE(PSIX);
	psq_store_pair(hCPU, EA, frD, type, scale, PSWX != 0);

	PPCInterpreter_nextInstruction(hCPU);
}

static void PPCInterpreter_PSQ_L(PPCInterpreter_t* hCPU, unsigned int opcode)
{
	FPUCheckAvailable();
	// next instruction
	PPCInterpreter_nextInstruction(hCPU);

	const sint32 frD = (opcode >> 21) & 0x1F;
	const sint32 rA = (opcode >> 16) & 0x1F;
	uint32 data0 = 0, data1 = 0;
	const sint32 type = LD_TYPE(PSI);
	const uint8 scale = (uint8)LD_SCALE(PSI);

	// 12-bit signed displacement (not 11, and not the full 16-bit field).
	const uint32 EA = (uint32)psq_d_form_disp(opcode) + (rA ? hCPU->gpr[rA] : 0);

	if (opcode & 0x8000)
	{
		if ((type == 4) || (type == 6)) *(uint8*)&data0 = ppcItpCtrl::ppcMem_readDataU8(hCPU, EA);
		else if ((type == 5) || (type == 7)) *(uint16*)&data0 = ppcItpCtrl::ppcMem_readDataU16(hCPU, EA);
		else *(uint32*)&data0 = ppcItpCtrl::ppcMem_readDataU32(hCPU, EA);
		if (type == 6) if (data0 & 0x80) data0 |= 0xffffff00;
		if (type == 7) if (data0 & 0x8000) data0 |= 0xffff0000;

		hCPU->fpr[frD].fp0 = dequantize_to_double(data0, type, scale);
		hCPU->fpr[frD].fp1 = 1.0f;
	}
	else
	{
		if ((type == 4) || (type == 6))
		{
			*(uint8*)&data0 = ppcItpCtrl::ppcMem_readDataU8(hCPU, EA);
			*(uint8*)&data1 = ppcItpCtrl::ppcMem_readDataU8(hCPU, EA + 1);
		}
		else if ((type == 5) || (type == 7))
		{
			*(uint16*)&data0 = ppcItpCtrl::ppcMem_readDataU16(hCPU, EA);
			*(uint16*)&data1 = ppcItpCtrl::ppcMem_readDataU16(hCPU, EA + 2);
		}
		else
		{
			*(uint32*)&data0 = ppcItpCtrl::ppcMem_readDataU32(hCPU, EA);
			*(uint32*)&data1 = ppcItpCtrl::ppcMem_readDataU32(hCPU, EA + 4);
		}
		if (type == 6)
		{
			if (data0 & 0x80) data0 |= 0xffffff00;
			if (data1 & 0x80) data1 |= 0xffffff00;
		}
		if (type == 7)
		{
			if (data0 & 0x8000) data0 |= 0xffff0000;
			if (data1 & 0x8000) data1 |= 0xffff0000;
		}

		hCPU->fpr[frD].fp0 = dequantize_to_double(data0, type, scale);
		hCPU->fpr[frD].fp1 = dequantize_to_double(data1, type, scale);
	}
}

static void PPCInterpreter_PSQ_LU(PPCInterpreter_t* hCPU, unsigned int opcode)
{
	FPUCheckAvailable();
	// next instruction
	PPCInterpreter_nextInstruction(hCPU);

	const sint32 frD = (opcode >> 21) & 0x1F;
	const sint32 rA = (opcode >> 16) & 0x1F;
	uint32 data0 = 0, data1 = 0;
	const sint32 type = LD_TYPE(PSI);
	const uint8 scale = (uint8)LD_SCALE(PSI);

	uint32 EA = (uint32)psq_d_form_disp(opcode);
	if (rA)
	{
		EA += hCPU->gpr[rA];
		hCPU->gpr[rA] = EA;
	}

	if (opcode & 0x8000)
	{
		if ((type == 4) || (type == 6)) *(uint8*)&data0 = ppcItpCtrl::ppcMem_readDataU8(hCPU, EA);
		else if ((type == 5) || (type == 7)) *(uint16*)&data0 = ppcItpCtrl::ppcMem_readDataU16(hCPU, EA);
		else *(uint32*)&data0 = ppcItpCtrl::ppcMem_readDataU32(hCPU, EA);
		if (type == 6) if (data0 & 0x80) data0 |= 0xffffff00;
		if (type == 7) if (data0 & 0x8000) data0 |= 0xffff0000;

		hCPU->fpr[frD].fp0 = dequantize_to_double(data0, type, scale);
		hCPU->fpr[frD].fp1 = 1.0f;
	}
	else
	{
		if ((type == 4) || (type == 6)) *(uint8*)&data0 = ppcItpCtrl::ppcMem_readDataU8(hCPU, EA);
		else if ((type == 5) || (type == 7)) *(uint16*)&data0 = ppcItpCtrl::ppcMem_readDataU16(hCPU, EA);
		else *(uint32*)&data0 = ppcItpCtrl::ppcMem_readDataU32(hCPU, EA);
		if (type == 6) if (data0 & 0x80) data0 |= 0xffffff00;
		if (type == 7) if (data0 & 0x8000) data0 |= 0xffff0000;

		if ((type == 4) || (type == 6)) *(uint8*)&data1 = ppcItpCtrl::ppcMem_readDataU8(hCPU, EA + 1);
		else if ((type == 5) || (type == 7)) *(uint16*)&data1 = ppcItpCtrl::ppcMem_readDataU16(hCPU, EA + 2);
		else *(uint32*)&data1 = ppcItpCtrl::ppcMem_readDataU32(hCPU, EA + 4);
		if (type == 6) if (data1 & 0x80) data1 |= 0xffffff00;
		if (type == 7) if (data1 & 0x8000) data1 |= 0xffff0000;

		hCPU->fpr[frD].fp0 = dequantize_to_double(data0, type, scale);
		hCPU->fpr[frD].fp1 = dequantize_to_double(data1, type, scale);
	}
}

// X-form psq_lx (XO=6) and psq_lux (XO=38). Update form selected by bit 6.
static void PPCInterpreter_PSQ_LX(PPCInterpreter_t* hCPU, unsigned int opcode)
{
	FPUCheckAvailable();

	const sint32 frD = (opcode >> 21) & 0x1F;
	const uint32 rA = (opcode >> 16) & 0x1F;
	const uint32 rB = (opcode >> 11) & 0x1F;
	// X-form EA: rA==0 means zero; rB always gpr[rB].
	const uint32 EA = (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB];
	const bool withUpdate = (opcode & 0x40) != 0; // psq_lux
	if (withUpdate && rA)
		hCPU->gpr[rA] = EA;

	uint32 data0 = 0, data1 = 0;
	const sint32 type = LD_TYPE(PSIX);
	const uint8 scale = (uint8)LD_SCALE(PSIX);

	if (PSWX)
	{
		if ((type == 4) || (type == 6)) *(uint8*)&data0 = ppcItpCtrl::ppcMem_readDataU8(hCPU, EA);
		else if ((type == 5) || (type == 7)) *(uint16*)&data0 = ppcItpCtrl::ppcMem_readDataU16(hCPU, EA);
		else *(uint32*)&data0 = ppcItpCtrl::ppcMem_readDataU32(hCPU, EA);
		if (type == 6) if (data0 & 0x80) data0 |= 0xffffff00;
		if (type == 7) if (data0 & 0x8000) data0 |= 0xffff0000;

		hCPU->fpr[frD].fp0 = dequantize_to_double(data0, type, scale);
		hCPU->fpr[frD].fp1 = 1.0f;
	}
	else
	{
		if ((type == 4) || (type == 6)) *(uint8*)&data0 = ppcItpCtrl::ppcMem_readDataU8(hCPU, EA);
		else if ((type == 5) || (type == 7)) *(uint16*)&data0 = ppcItpCtrl::ppcMem_readDataU16(hCPU, EA);
		else *(uint32*)&data0 = ppcItpCtrl::ppcMem_readDataU32(hCPU, EA);
		if (type == 6) if (data0 & 0x80) data0 |= 0xffffff00;
		if (type == 7) if (data0 & 0x8000) data0 |= 0xffff0000;

		if ((type == 4) || (type == 6)) *(uint8*)&data1 = ppcItpCtrl::ppcMem_readDataU8(hCPU, EA + 1);
		else if ((type == 5) || (type == 7)) *(uint16*)&data1 = ppcItpCtrl::ppcMem_readDataU16(hCPU, EA + 2);
		else *(uint32*)&data1 = ppcItpCtrl::ppcMem_readDataU32(hCPU, EA + 4);
		if (type == 6) if (data1 & 0x80) data1 |= 0xffffff00;
		if (type == 7) if (data1 & 0x8000) data1 |= 0xffff0000;

		hCPU->fpr[frD].fp0 = dequantize_to_double(data0, type, scale);
		hCPU->fpr[frD].fp1 = dequantize_to_double(data1, type, scale);
	}

	PPCInterpreter_nextInstruction(hCPU);
}

// misc

static void PPCInterpreter_DCBZ(PPCInterpreter_t* hCPU, uint32 Opcode)
{
	int rA, rB;
	rA = (Opcode >> (31 - 15)) & 0x1F;
	rB = (Opcode >> (31 - 20)) & 0x1F;

	uint32 ea = (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB];
	ea &= ~31;
	if constexpr(ppcItpCtrl::allowSupervisorMode)
	{
		// todo - optimize
		ppcItpCtrl::ppcMem_writeDataU32(hCPU, ea + 0, 0);
		DSI_EXIT();
		ppcItpCtrl::ppcMem_writeDataU32(hCPU, ea + 4, 0);
		ppcItpCtrl::ppcMem_writeDataU32(hCPU, ea + 8, 0);
		ppcItpCtrl::ppcMem_writeDataU32(hCPU, ea + 12, 0);
		ppcItpCtrl::ppcMem_writeDataU32(hCPU, ea + 16, 0);
		ppcItpCtrl::ppcMem_writeDataU32(hCPU, ea + 20, 0);
		ppcItpCtrl::ppcMem_writeDataU32(hCPU, ea + 24, 0);
		ppcItpCtrl::ppcMem_writeDataU32(hCPU, ea + 28, 0);
	}
	else
	{
		memset((void*)memory_getPointerFromVirtualOffset(ea), 0x00, 0x20);
	}

	// debug output
#ifdef __DEBUG_OUTPUT_INSTRUCTION
	debug_printf("DCBZ\n");
#endif
	// next instruction
	PPCInterpreter_nextInstruction(hCPU);
}