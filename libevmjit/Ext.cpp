#include "Ext.h"

#include "preprocessor/llvm_includes_start.h"
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include "preprocessor/llvm_includes_end.h"

#include "RuntimeManager.h"
#include "Memory.h"
#include "Type.h"
#include "Endianness.h"

namespace dev
{
namespace eth
{
namespace jit
{

Ext::Ext(RuntimeManager& _runtimeManager, Memory& _memoryMan) :
	RuntimeHelper(_runtimeManager),
	m_memoryMan(_memoryMan)
{
	m_funcs = decltype(m_funcs)();
	m_argAllocas = decltype(m_argAllocas)();
	m_size = m_builder.CreateAlloca(Type::Size, nullptr, "env.size");
}

namespace
{

using FuncDesc = std::tuple<char const*, llvm::FunctionType*>;

llvm::FunctionType* getFunctionType(llvm::Type* _returnType, std::initializer_list<llvm::Type*> const& _argsTypes)
{
	return llvm::FunctionType::get(_returnType, llvm::ArrayRef<llvm::Type*>{_argsTypes.begin(), _argsTypes.size()}, false);
}

std::array<FuncDesc, sizeOf<EnvFunc>::value> const& getEnvFuncDescs()
{
	static std::array<FuncDesc, sizeOf<EnvFunc>::value> descs{{
		FuncDesc{"env_sload",   getFunctionType(Type::Void, {Type::EnvPtr, Type::WordPtr, Type::WordPtr})},
		FuncDesc{"env_sstore",  getFunctionType(Type::Void, {Type::EnvPtr, Type::WordPtr, Type::WordPtr})},
		FuncDesc{"env_sha3", getFunctionType(Type::Void, {Type::BytePtr, Type::Size, Type::WordPtr})},
		FuncDesc{"env_balance", getFunctionType(Type::Void, {Type::WordPtr, Type::EnvPtr, Type::WordPtr})},
		FuncDesc{"env_create", getFunctionType(Type::Void, {Type::EnvPtr, Type::GasPtr, Type::WordPtr, Type::BytePtr, Type::Size, Type::WordPtr})},
		FuncDesc{"env_call", getFunctionType(Type::Bool, {Type::EnvPtr, Type::GasPtr, Type::Gas, Type::WordPtr, Type::WordPtr, Type::WordPtr, Type::WordPtr, Type::WordPtr, Type::BytePtr, Type::Size, Type::BytePtr, Type::Size})},
		FuncDesc{"env_log", getFunctionType(Type::Void, {Type::EnvPtr, Type::BytePtr, Type::Size, Type::WordPtr, Type::WordPtr, Type::WordPtr, Type::WordPtr})},
		FuncDesc{"env_blockhash", getFunctionType(Type::Void, {Type::EnvPtr, Type::WordPtr, Type::WordPtr})},
		FuncDesc{"env_extcode", getFunctionType(Type::BytePtr, {Type::EnvPtr, Type::WordPtr, Type::Size->getPointerTo()})},
	}};

	return descs;
}

llvm::Function* createFunc(EnvFunc _id, llvm::Module* _module)
{
	auto&& desc = getEnvFuncDescs()[static_cast<size_t>(_id)];
	return llvm::Function::Create(std::get<1>(desc), llvm::Function::ExternalLinkage, std::get<0>(desc), _module);
}

llvm::Function* getQueryFunc(llvm::Module* _module)
{
	static const auto funcName = "evm.query";
	auto func = _module->getFunction(funcName);
	if (!func)
	{
		// TODO: Mark the function as pure to eliminate multiple calls.
		auto i32 = llvm::IntegerType::getInt32Ty(_module->getContext());
		auto addrTy = llvm::IntegerType::get(_module->getContext(), 160);
		auto fty = llvm::FunctionType::get(
				Type::Void, {Type::WordPtr, Type::EnvPtr, i32, addrTy->getPointerTo(), Type::WordPtr}, false);
		func = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, funcName, _module);
		func->addAttribute(1, llvm::Attribute::NoAlias);
		func->addAttribute(1, llvm::Attribute::NoCapture);
		func->addAttribute(4, llvm::Attribute::ReadOnly);
		func->addAttribute(4, llvm::Attribute::NoAlias);
		func->addAttribute(4, llvm::Attribute::NoCapture);
		func->addAttribute(5, llvm::Attribute::ReadOnly);
		func->addAttribute(5, llvm::Attribute::NoAlias);
		func->addAttribute(5, llvm::Attribute::NoCapture);
	}
	return func;
}

llvm::Function* getUpdateFunc(llvm::Module* _module)
{
	static const auto funcName = "evm.update";
	auto func = _module->getFunction(funcName);
	if (!func)
	{
		auto i32 = llvm::IntegerType::getInt32Ty(_module->getContext());
		auto fty = llvm::FunctionType::get(Type::Void, {Type::EnvPtr, i32, Type::WordPtr, Type::WordPtr}, false);
		func = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, funcName, _module);
		func->addAttribute(3, llvm::Attribute::ReadOnly);
		func->addAttribute(3, llvm::Attribute::NoAlias);
		func->addAttribute(3, llvm::Attribute::NoCapture);
		func->addAttribute(4, llvm::Attribute::ReadOnly);
		func->addAttribute(4, llvm::Attribute::NoAlias);
		func->addAttribute(4, llvm::Attribute::NoCapture);
	}
	return func;
}

llvm::StructType* getMemRefTy(llvm::Module* _module)
{
	static const auto name = "evm.memref";
	auto ty = _module->getTypeByName(name);
	if (!ty)
		ty = llvm::StructType::create({Type::BytePtr, Type::Size}, name);
	return ty;
}

llvm::Function* getCallFunc(llvm::Module* _module)
{
	static const auto funcName = "call";
	auto func = _module->getFunction(funcName);
	if (!func)
	{
		auto i32 = llvm::IntegerType::getInt32Ty(_module->getContext());
		auto addrTy = llvm::IntegerType::get(_module->getContext(), 160);
		auto addrPtrTy = addrTy->getPointerTo();
		auto fty = llvm::FunctionType::get(
			Type::Gas,
			{Type::EnvPtr, i32, Type::Gas, addrPtrTy, Type::WordPtr, Type::BytePtr, Type::Size, Type::BytePtr, Type::Size},
			false);
		func = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "evm.call", _module);
		func->addAttribute(4, llvm::Attribute::ReadOnly);
		func->addAttribute(4, llvm::Attribute::NoAlias);
		func->addAttribute(4, llvm::Attribute::NoCapture);
		func->addAttribute(5, llvm::Attribute::ReadOnly);
		func->addAttribute(5, llvm::Attribute::NoAlias);
		func->addAttribute(5, llvm::Attribute::NoCapture);
		func->addAttribute(6, llvm::Attribute::ReadOnly);
		func->addAttribute(6, llvm::Attribute::NoCapture);
		func->addAttribute(8, llvm::Attribute::NoCapture);
		auto callFunc = func;

		// Create a call wrapper to handle additional checks.
		fty = llvm::FunctionType::get(
			Type::Gas,
			{Type::EnvPtr, i32, Type::Gas, addrPtrTy, Type::WordPtr, Type::BytePtr, Type::Size, Type::BytePtr, Type::Size, addrTy, Type::Size},
			false
		);
		func = llvm::Function::Create(fty, llvm::Function::PrivateLinkage, funcName, _module);
		func->addAttribute(4, llvm::Attribute::ReadOnly);
		func->addAttribute(4, llvm::Attribute::NoAlias);
		func->addAttribute(4, llvm::Attribute::NoCapture);
		func->addAttribute(5, llvm::Attribute::ReadOnly);
		func->addAttribute(5, llvm::Attribute::NoAlias);
		func->addAttribute(5, llvm::Attribute::NoCapture);
		func->addAttribute(6, llvm::Attribute::ReadOnly);
		func->addAttribute(6, llvm::Attribute::NoCapture);
		func->addAttribute(8, llvm::Attribute::NoCapture);

		auto iter = func->arg_begin();
		auto& env = *iter;
		std::advance(iter, 1);
		auto& callKind = *iter;
		std::advance(iter, 1);
		auto& gas = *iter;
		std::advance(iter, 2);
		auto& valuePtr = *iter;
		std::advance(iter, 5);
		auto& addr = *iter;
		std::advance(iter, 1);
		auto& depth = *iter;


		auto& ctx = _module->getContext();
		llvm::IRBuilder<> builder(ctx);
		auto entryBB = llvm::BasicBlock::Create(ctx, "Entry", func);
		auto checkTransferBB = llvm::BasicBlock::Create(ctx, "CheckTransfer", func);
		auto checkBalanceBB = llvm::BasicBlock::Create(ctx, "CheckBalance", func);
		auto callBB = llvm::BasicBlock::Create(ctx, "Call", func);
		auto failBB = llvm::BasicBlock::Create(ctx, "Fail", func);

		builder.SetInsertPoint(entryBB);
		auto v = builder.CreateAlloca(Type::Word);
		auto addrAlloca = builder.CreateBitCast(builder.CreateAlloca(Type::Word), addrPtrTy);
		auto queryFn = getQueryFunc(_module);
		auto depthOk = builder.CreateICmpSLT(&depth, builder.getInt64(1024));
		builder.CreateCondBr(depthOk, checkTransferBB, failBB);

		builder.SetInsertPoint(checkTransferBB);
		auto notDelegateCall = builder.CreateICmpNE(&callKind, builder.getInt32(EVM_DELEGATECALL));
		llvm::Value* value = builder.CreateLoad(&valuePtr);
		auto valueNonZero = builder.CreateICmpNE(value, Constant::get(0));
		auto transfer = builder.CreateAnd(notDelegateCall, valueNonZero);
		builder.CreateCondBr(transfer, checkBalanceBB, callBB);

		builder.SetInsertPoint(checkBalanceBB);
		builder.CreateStore(&addr, addrAlloca);
		auto null = llvm::ConstantPointerNull::get(Type::WordPtr);
		builder.CreateCall(queryFn, {v, &env, builder.getInt32(EVM_BALANCE), addrAlloca, null});
		llvm::Value* balance = builder.CreateLoad(v);
		balance = Endianness::toNative(builder, balance);
		value = Endianness::toNative(builder, value);
		auto balanceOk = builder.CreateICmpUGE(balance, value);
		builder.CreateCondBr(balanceOk, callBB, failBB);

		builder.SetInsertPoint(callBB);
		// Pass the first 9 args to the external call.
		llvm::Value* args[9];
		auto it = func->arg_begin();
		for (auto outIt = std::begin(args); outIt != std::end(args); ++it, ++outIt)
			*outIt = &*it;
		auto ret = builder.CreateCall(callFunc, args);
		builder.CreateRet(ret);

		builder.SetInsertPoint(failBB);
		auto failRet = builder.CreateOr(&gas, builder.getInt64(EVM_CALL_FAILURE));
		builder.CreateRet(failRet);
	}
	return func;
}

llvm::Function* getBlockHashFunc(llvm::Module* _module)
{
	static const auto funcName = "evm.blockhash";
	auto func = _module->getFunction(funcName);
	if (!func)
	{
		// TODO: Mark the function as pure to eliminate multiple calls.
		auto i64 = llvm::IntegerType::getInt64Ty(_module->getContext());
		auto fty = llvm::FunctionType::get(Type::Void, {Type::WordPtr, Type::EnvPtr, i64}, false);
		func = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, funcName, _module);
		func->addAttribute(1, llvm::Attribute::NoAlias);
		func->addAttribute(1, llvm::Attribute::NoCapture);
	}
	return func;
}

}



llvm::Value* Ext::getArgAlloca()
{
	auto& a = m_argAllocas[m_argCounter];
	if (!a)
	{
		InsertPointGuard g{m_builder};
		auto allocaIt = getMainFunction()->front().begin();
		auto allocaPtr = &(*allocaIt);
		std::advance(allocaIt, m_argCounter); // Skip already created allocas
		m_builder.SetInsertPoint(allocaPtr);
		a = m_builder.CreateAlloca(Type::Word, nullptr, {"a.", std::to_string(m_argCounter)});
	}
	++m_argCounter;
	return a;
}

llvm::CallInst* Ext::createCall(EnvFunc _funcId, std::initializer_list<llvm::Value*> const& _args)
{
	auto& func = m_funcs[static_cast<size_t>(_funcId)];
	if (!func)
		func = createFunc(_funcId, getModule());

	m_argCounter = 0;
	return m_builder.CreateCall(func, {_args.begin(), _args.size()});
}

llvm::Value* Ext::createCABICall(llvm::Function* _func, std::initializer_list<llvm::Value*> const& _args)
{
	auto args = llvm::SmallVector<llvm::Value*, 8>{_args};
	for (auto&& farg: _func->args())
	{
		if (farg.hasByValAttr() || farg.getType()->isPointerTy())
		{
			auto& arg = args[farg.getArgNo()];
			// TODO: Remove defensive check and always use it this way.
			if (!arg->getType()->isPointerTy())
			{
				auto mem = getArgAlloca();
				// TODO: The bitcast may be redundant
				mem = m_builder.CreateBitCast(mem, arg->getType()->getPointerTo());
				m_builder.CreateStore(arg, mem);
				arg = mem;
			}
		}
	}

	m_argCounter = 0;
	return m_builder.CreateCall(_func, args);
}

llvm::Value* Ext::sload(llvm::Value* _index)
{
	auto index = Endianness::toBE(m_builder, _index);
	auto addrTy = m_builder.getIntNTy(160);
	auto address = m_builder.CreateTrunc(Endianness::toBE(m_builder, getRuntimeManager().getAddress()), addrTy);
	auto pAddr = m_builder.CreateBitCast(getArgAlloca(), addrTy->getPointerTo());
	m_builder.CreateStore(address, pAddr);
	auto func = getQueryFunc(getModule());
	auto pValue = getArgAlloca();
	createCABICall(func, {pValue, getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_SLOAD), pAddr, index});
	return Endianness::toNative(m_builder, m_builder.CreateLoad(pValue));
}

void Ext::sstore(llvm::Value* _index, llvm::Value* _value)
{
	auto index = Endianness::toBE(m_builder, _index);
	auto value = Endianness::toBE(m_builder, _value);
	auto func = getUpdateFunc(getModule());
	createCABICall(func, {getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_SSTORE), index, value});
}

void Ext::selfdestruct(llvm::Value* _beneficiary)
{
	auto func = getUpdateFunc(getModule());
	auto b = Endianness::toBE(m_builder, _beneficiary);
	auto undef = llvm::UndefValue::get(Type::WordPtr);
	createCABICall(func, {getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_SELFDESTRUCT), b, undef});
}

llvm::Value* Ext::calldataload(llvm::Value* _idx)
{
	auto ret = getArgAlloca();
	auto result = m_builder.CreateBitCast(ret, Type::BytePtr);

	auto callDataSize = getRuntimeManager().getCallDataSize();
	auto callDataSize64 = m_builder.CreateTrunc(callDataSize, Type::Size);
	auto idxValid = m_builder.CreateICmpULT(_idx, callDataSize);
	auto idx = m_builder.CreateTrunc(m_builder.CreateSelect(idxValid, _idx, callDataSize), Type::Size, "idx");

	auto end = m_builder.CreateNUWAdd(idx, m_builder.getInt64(32));
	end = m_builder.CreateSelect(m_builder.CreateICmpULE(end, callDataSize64), end, callDataSize64);
	auto copySize = m_builder.CreateNUWSub(end, idx);
	auto padSize = m_builder.CreateNUWSub(m_builder.getInt64(32), copySize);
	auto dataBegin = m_builder.CreateGEP(Type::Byte, getRuntimeManager().getCallData(), idx);
	m_builder.CreateMemCpy(result, dataBegin, copySize, 1);
	auto pad = m_builder.CreateGEP(Type::Byte, result, copySize);
	m_builder.CreateMemSet(pad, m_builder.getInt8(0), padSize, 1);

	m_argCounter = 0; // Release args allocas. TODO: This is a bad design
	return Endianness::toNative(m_builder, m_builder.CreateLoad(ret));
}

llvm::Value* Ext::balance(llvm::Value* _address)
{
	auto func = getQueryFunc(getModule());
	auto addrTy = m_builder.getIntNTy(160);
	auto address = Endianness::toBE(m_builder, m_builder.CreateTrunc(_address, addrTy));
	auto pResult = getArgAlloca();
	auto pAddr = m_builder.CreateBitCast(getArgAlloca(), addrTy->getPointerTo());
	m_builder.CreateStore(address, pAddr);
	auto null = llvm::ConstantPointerNull::get(Type::WordPtr);
	createCABICall(func, {pResult, getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_BALANCE), pAddr, null});
	return Endianness::toNative(m_builder, m_builder.CreateLoad(pResult));
}

llvm::Value* Ext::exists(llvm::Value* _address)
{
	auto func = getQueryFunc(getModule());
	auto addrTy = m_builder.getIntNTy(160);
	auto address = Endianness::toBE(m_builder, m_builder.CreateTrunc(_address, addrTy));
	auto pResult = getArgAlloca();
	auto pAddr = m_builder.CreateBitCast(getArgAlloca(), addrTy->getPointerTo());
	m_builder.CreateStore(address, pAddr);
	auto null = llvm::ConstantPointerNull::get(Type::WordPtr);
	createCABICall(func, {pResult, getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_ACCOUNT_EXISTS), pAddr, null});
	return m_builder.CreateTrunc(m_builder.CreateLoad(pResult), m_builder.getInt1Ty());
}

llvm::Value* Ext::blockHash(llvm::Value* _number)
{
	auto func = getBlockHashFunc(getModule());
	auto number = m_builder.CreateTrunc(_number, m_builder.getInt64Ty());
	auto pResult = getArgAlloca();
	createCABICall(func, {pResult, getRuntimeManager().getEnvPtr(), number});
	return Endianness::toNative(m_builder, m_builder.CreateLoad(pResult));
}

llvm::Value* Ext::sha3(llvm::Value* _inOff, llvm::Value* _inSize)
{
	auto begin = m_memoryMan.getBytePtr(_inOff);
	auto size = m_builder.CreateTrunc(_inSize, Type::Size, "size");
	auto ret = getArgAlloca();
	createCall(EnvFunc::sha3, {begin, size, ret});
	llvm::Value* hash = m_builder.CreateLoad(ret);
	return Endianness::toNative(m_builder, hash);
}

MemoryRef Ext::extcode(llvm::Value* _address)
{
	auto func = getQueryFunc(getModule());
	auto addrTy = m_builder.getIntNTy(160);
	auto address = Endianness::toBE(m_builder, m_builder.CreateTrunc(_address, addrTy));
	auto pAddr = m_builder.CreateBitCast(getArgAlloca(), addrTy->getPointerTo());
	m_builder.CreateStore(address, pAddr);
	auto null = llvm::ConstantPointerNull::get(Type::WordPtr);
	auto vPtr = getArgAlloca();
	createCABICall(func, {vPtr, getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_CODE_BY_ADDRESS), pAddr, null});
	auto memRefTy = getMemRefTy(getModule());
	auto memRefPtr = m_builder.CreateBitCast(vPtr, memRefTy->getPointerTo());
	auto memRef = m_builder.CreateLoad(memRefTy, memRefPtr, "memref");
	auto code = m_builder.CreateExtractValue(memRef, 0, "code");
	auto size = m_builder.CreateExtractValue(memRef, 1, "codesize");
	auto size256 = m_builder.CreateZExt(size, Type::Word);
	return {code, size256};
}

llvm::Value* Ext::extcodesize(llvm::Value* _address)
{
	auto func = getQueryFunc(getModule());
	auto addrTy = m_builder.getIntNTy(160);
	auto address = Endianness::toBE(m_builder, m_builder.CreateTrunc(_address, addrTy));
	auto pAddr = m_builder.CreateBitCast(getArgAlloca(), addrTy->getPointerTo());
	m_builder.CreateStore(address, pAddr);
	auto null = llvm::ConstantPointerNull::get(Type::WordPtr);
	auto vPtr = getArgAlloca();
	createCABICall(func, {vPtr, getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_CODE_SIZE), pAddr, null});
	auto int64ty = m_builder.getInt64Ty();
	auto sizePtr = m_builder.CreateBitCast(vPtr, int64ty->getPointerTo());
	auto size = m_builder.CreateLoad(int64ty, sizePtr, "codesize");
	return m_builder.CreateZExt(size, Type::Word);
}

void Ext::log(llvm::Value* _memIdx, llvm::Value* _numBytes, llvm::ArrayRef<llvm::Value*> _topics)
{
	if (!m_topics)
	{
		InsertPointGuard g{m_builder};
		auto& entryBB = getMainFunction()->front();
		m_builder.SetInsertPoint(&entryBB, entryBB.begin());
		m_topics = m_builder.CreateAlloca(Type::Word, m_builder.getInt32(4), "topics");
	}

	auto begin = m_memoryMan.getBytePtr(_memIdx);
	auto size = m_builder.CreateTrunc(_numBytes, Type::Size, "size");

	for (size_t i = 0; i < _topics.size(); ++i)
	{
		auto t = Endianness::toBE(m_builder, _topics[i]);
		auto p = m_builder.CreateConstGEP1_32(m_topics, static_cast<unsigned>(i));
		m_builder.CreateStore(t, p);
	}

	auto func = getUpdateFunc(getModule());
	auto a = getArgAlloca();
	auto memRefTy = getMemRefTy(getModule());
	auto pMemRef = m_builder.CreateBitCast(a, memRefTy->getPointerTo());
	auto pData = m_builder.CreateConstGEP2_32(memRefTy, pMemRef, 0, 0, "log.data");
	m_builder.CreateStore(begin, pData);
	auto pSize = m_builder.CreateConstGEP2_32(memRefTy, pMemRef, 0, 1, "log.size");
	m_builder.CreateStore(size, pSize);

	auto b = getArgAlloca();
	pMemRef = m_builder.CreateBitCast(b, memRefTy->getPointerTo());
	pData = m_builder.CreateConstGEP2_32(memRefTy, pMemRef, 0, 0, "topics.data");
	m_builder.CreateStore(m_builder.CreateBitCast(m_topics, m_builder.getInt8PtrTy()), pData);
	pSize = m_builder.CreateConstGEP2_32(memRefTy, pMemRef, 0, 1, "topics.size");
	m_builder.CreateStore(m_builder.getInt64(_topics.size() * 32), pSize);

	createCABICall(func, {getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_LOG), a, b});
}

llvm::Value* Ext::call(evm_call_kind _kind,
					   llvm::Value* _gas,
					   llvm::Value* _addr,
					   llvm::Value* _value,
					   llvm::Value* _inOff,
					   llvm::Value* _inSize,
					   llvm::Value* _outOff,
					   llvm::Value* _outSize)
{
	auto gas = m_builder.CreateTrunc(_gas, Type::Size);
	auto addrTy = m_builder.getIntNTy(160);
	auto addr = m_builder.CreateTrunc(_addr, addrTy);
	addr = Endianness::toBE(m_builder, addr);
	auto inData = m_memoryMan.getBytePtr(_inOff);
	auto inSize = m_builder.CreateTrunc(_inSize, Type::Size);
	auto outData = m_memoryMan.getBytePtr(_outOff);
	auto outSize = m_builder.CreateTrunc(_outSize, Type::Size);

	auto value = getArgAlloca();
	m_builder.CreateStore(Endianness::toBE(m_builder, _value), value);

	auto func = getCallFunc(getModule());
	auto myAddr = Endianness::toBE(m_builder, m_builder.CreateTrunc(Endianness::toNative(m_builder, getRuntimeManager().getAddress()), addrTy));
	return createCABICall(
		func, {getRuntimeManager().getEnvPtr(), m_builder.getInt32(_kind), gas,
			   addr, value, inData, inSize, outData, outSize,
			   myAddr, getRuntimeManager().getDepth()});
}

std::tuple<llvm::Value*, llvm::Value*> Ext::create(llvm::Value* _gas,
                                                   llvm::Value* _endowment,
												   llvm::Value* _initOff,
												   llvm::Value* _initSize)
{
	auto addrTy = m_builder.getIntNTy(160);
	auto value = getArgAlloca();
	m_builder.CreateStore(Endianness::toBE(m_builder, _endowment), value);
	auto inData = m_memoryMan.getBytePtr(_initOff);
	auto inSize = m_builder.CreateTrunc(_initSize, Type::Size);
	auto pAddr =
		m_builder.CreateBitCast(getArgAlloca(), m_builder.getInt8PtrTy());

	auto func = getCallFunc(getModule());
	auto myAddr = Endianness::toBE(m_builder, m_builder.CreateTrunc(Endianness::toNative(m_builder, getRuntimeManager().getAddress()), addrTy));
	auto ret = createCABICall(
		func, {getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_CREATE),
			   _gas, llvm::UndefValue::get(addrTy), value, inData, inSize, pAddr,
			   m_builder.getInt64(20), myAddr, getRuntimeManager().getDepth()});

	pAddr = m_builder.CreateBitCast(pAddr, addrTy->getPointerTo());
	return std::tuple<llvm::Value*, llvm::Value*>{ret, pAddr};
}
}
}
}
