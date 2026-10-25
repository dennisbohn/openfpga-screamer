// VexRiscv CPUs for the Screamer core (Analogue Pocket, Cyclone V 5CEBA4F23C8).
//
// RV32IM, 5-stage pipeline with full bypassing, single-cycle barrel shifter, MulPlugin (DSP),
// DivPlugin (iterative, 34 cycles), direct-mapped 8 KB I$ and 8 KB write-through D$ (32-byte
// lines), no MMU (StaticMemoryTranslatorPlugin: 0xF0000000-0xFFFFFFFF is uncached I/O), no debug
// plugin, machine-mode CSRs with interrupts plus rdcycle, native iBus/dBus (cmd stream + rsp flow).
// Variants: im, imf (+ RV32F: FpuPlugin with the FpuCore inside the CPU) and imfc (+ CfuPlugin:
// custom-0 to a CFU bus on the top level). The core uses imfc; docs/soc.md is the contract.
//
// tools/vexriscv/gen.sh copies this file into a VexRiscv clone (src/main/scala/pocket/) and runs
//   sbt "runMain pocket.PocketVexGen <outDir> [variant ...]"     (no variant: all of them)
// Output: <outDir>/<variant>/<Module>.v and <Module>.yaml
package pocket

import java.nio.file.{Files, Paths}

import scala.collection.mutable.ArrayBuffer

import spinal.core._
import vexriscv.{VexRiscv, VexRiscvConfig, plugin}
import vexriscv.plugin._
import vexriscv.ip.{DataCacheConfig, InstructionCacheConfig}
import vexriscv.ip.fpu.FpuParameter

case class PocketVexOptions(
  module : String,                      // Verilog module name
  fpu    : Boolean = false,             // RV32F in hardware
  cfu    : Boolean = false              // CfuPlugin with a CFU bus on the top level
)

object PocketVex {
  def plugins(o : PocketVexOptions, yamlPath : String) : Seq[Plugin[VexRiscv]] = {
    val list = ArrayBuffer[Plugin[VexRiscv]]()
    list += new IBusCachedPlugin(
      resetVector          = 0x00000000l,
      relaxedPcCalculation = false,
      prediction           = STATIC,
      historyRamSizeLog2   = 8,
      compressedGen        = false,
      config = InstructionCacheConfig(
        cacheSize          = 8192,
        bytePerLine        = 32,
        wayCount           = 1,
        addressWidth       = 32,
        cpuDataWidth       = 32,
        memDataWidth       = 32,
        catchIllegalAccess = true,
        catchAccessFault   = true,
        asyncTagMemory     = false,
        twoCycleRam        = false,
        twoCycleCache      = true
      )
    )
    list += new DBusCachedPlugin(
      config = new DataCacheConfig(
        cacheSize        = 8192,
        bytePerLine      = 32,
        wayCount         = 1,
        addressWidth     = 32,
        cpuDataWidth     = 32,
        memDataWidth     = 32,
        catchAccessError = true,
        catchIllegal     = true,
        catchUnaligned   = true
      )
    )
    list += new StaticMemoryTranslatorPlugin(ioRange = _(31 downto 28) === 0xF)
    list += new DecoderSimplePlugin(catchIllegalInstruction = true)
    list += new RegFilePlugin(regFileReadyKind = plugin.SYNC, zeroBoot = false)
    list += new IntAluPlugin
    list += new SrcPlugin(separatedAddSub = false, executeInsertion = true)
    list += new FullBarrelShifterPlugin
    list += new HazardSimplePlugin(
      bypassExecute           = true,
      bypassMemory            = true,
      bypassWriteBack         = true,
      bypassWriteBackBuffer   = true,
      pessimisticUseSrc       = false,
      pessimisticWriteRegFile = false,
      pessimisticAddressMatch = false
    )
    list += new MulPlugin
    list += new DivPlugin
    list += new CsrPlugin(CsrPluginConfig.small(mtvecInit = 0x00000020l).copy(
      mtvecAccess  = CsrAccess.READ_WRITE,
      ecallGen     = true,
      ucycleAccess = CsrAccess.READ_ONLY            // rdcycle/rdcycleh for profiling
    ))
    list += new BranchPlugin(earlyBranch = false, catchAddressMisaligned = true)
    if (o.fpu) list += new FpuPlugin(
      externalFpu = false,
      p = FpuParameter(withDouble = false)          // mulWidthA/B = 18, sync RF, div and sqrt units
    )
    if (o.cfu) list += new CfuPlugin(
      stageCount       = 1,                         // issue in execute, result in memory (or later, stalls)
      allowZeroLatency = true,
      enableInit       = true,                      // no CSR write needed before the first CFU instruction
      encodings = List(
        CfuPluginEncoding(
          instruction = M"-------------------------0001011",   // custom-0, R-type
          functionId  = List(14 downto 12, 31 downto 25),      // function_id = {funct7, funct3}
          input2Kind  = CfuPlugin.Input2Kind.RS
        )
      ),
      busParameter = CfuBusParameter(
        CFU_VERSION                = 0,
        CFU_INTERFACE_ID_W         = 0,
        CFU_FUNCTION_ID_W          = 10,
        CFU_REORDER_ID_W           = 0,
        CFU_REQ_RESP_ID_W          = 0,
        CFU_INPUTS                 = 2,
        CFU_INPUT_DATA_W           = 32,
        CFU_OUTPUTS                = 1,
        CFU_OUTPUT_DATA_W          = 32,
        CFU_FLOW_REQ_READY_ALWAYS  = false,
        CFU_FLOW_RESP_READY_ALWAYS = false
      )
    )
    list += new YamlPlugin(yamlPath)
    list
  }

  def cpu(o : PocketVexOptions, yamlPath : String) : VexRiscv = {
    val cpu = new VexRiscv(VexRiscvConfig(plugins(o, yamlPath)))
    cpu.setDefinitionName(o.module)
    cpu
  }
}

object PocketVexGen {
  val variants : Map[String, PocketVexOptions] = Map(
    "im"   -> PocketVexOptions("VexPocketIM"),
    "imf"  -> PocketVexOptions("VexPocketIMF", fpu = true),
    "imfc" -> PocketVexOptions("VexPocketIMFC", fpu = true, cfu = true)      // the core's CPU
  )

  def main(args : Array[String]) : Unit = {
    val outRoot = args(0)
    val names = if (args.length > 1) args.drop(1).toSeq else variants.keys.toSeq.sorted
    for (name <- names) {
      val o = variants(name)
      val dir = s"$outRoot/$name"
      Files.createDirectories(Paths.get(dir))
      SpinalConfig(targetDirectory = dir).generateVerilog(PocketVex.cpu(o, s"$dir/${o.module}.yaml"))
    }
  }
}
