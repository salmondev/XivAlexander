#include "pch.h"

#include <XivAlexanderCommon/Sqex_Model.h>
#include <XivAlexanderCommon/Sqex_Sqpack_BinaryEntryProvider.h>
#include <XivAlexanderCommon/Sqex_Sqpack_Creator.h>
#include <XivAlexanderCommon/Sqex_Sqpack_EmptyOrObfuscatedEntryProvider.h>
#include <XivAlexanderCommon/Sqex_Sqpack_EntryProvider.h>
#include <XivAlexanderCommon/Sqex_Sqpack_EntryRawStream.h>
#include <XivAlexanderCommon/Sqex_Sqpack_ModelEntryProvider.h>
#include <XivAlexanderCommon/Sqex_Sqpack_RandomAccessStreamAsEntryProviderView.h>
#include <XivAlexanderCommon/Sqex_Sqpack_Reader.h>
#include <XivAlexanderCommon/Sqex_Sqpack_TextureEntryProvider.h>
#include <XivAlexanderCommon/Sqex_Texture_ModifiableTextureStream.h>
#include <XivAlexanderCommon/Sqex_ThirdParty_TexTools.h>
#include <XivAlexanderCommon/Utils_Win32_ThreadPool.h>

std::shared_ptr<Sqex::RandomAccessStream> StripSecondaryMipmaps(std::shared_ptr<Sqex::RandomAccessStream> src) {
	return src;

	auto stream = std::make_shared<Sqex::Texture::ModifiableTextureStream>(std::move(src));
	stream->TruncateMipmap(1);
	return stream;
}

std::shared_ptr<Sqex::RandomAccessStream> StripLodModels(std::shared_ptr<Sqex::RandomAccessStream> src) {
	return src;

	auto data = src->ReadStreamIntoVector<uint8_t>(0);
	auto& header = *reinterpret_cast<Sqex::Model::Header*>(&data[0]);
	header.VertexOffset[1] = header.VertexOffset[2] = header.IndexOffset[1] = header.IndexOffset[2] = 0;
	header.VertexSize[1] = header.VertexSize[2] = header.IndexSize[1] = header.IndexSize[2] = 0;
	header.LodCount = 1;
	data.resize(header.IndexOffset[0] + header.IndexSize[0]);
	src = std::make_shared<Sqex::MemoryRandomAccessStream>(data);
	return src;
}

std::shared_ptr<Sqex::Sqpack::EntryProvider> ToDecompressedEntryProvider(std::shared_ptr<Sqex::Sqpack::EntryProvider> src) {
	const auto& pathSpec = src->PathSpec();
	switch (src->EntryType()) {
		case Sqex::Sqpack::SqData::FileEntryType::EmptyOrObfuscated:
			if (src->ReadStream<Sqex::Sqpack::SqData::FileEntryHeader>(0).DecompressedSize == 0)
				return std::make_shared<Sqex::Sqpack::EmptyOrObfuscatedEntryProvider>(pathSpec);
			else
				return src;  // obfuscated; not applicable

		case Sqex::Sqpack::SqData::FileEntryType::Binary:
			return std::make_shared<Sqex::Sqpack::MemoryBinaryEntryProvider>(pathSpec, std::make_shared<Sqex::Sqpack::EntryRawStream>(std::move(src)), Z_NO_COMPRESSION);

		case Sqex::Sqpack::SqData::FileEntryType::Model:
			return std::make_shared<Sqex::Sqpack::MemoryModelEntryProvider>(pathSpec, StripLodModels(std::make_shared<Sqex::Sqpack::EntryRawStream>(std::move(src))), Z_NO_COMPRESSION);

		case Sqex::Sqpack::SqData::FileEntryType::Texture:
			return std::make_shared<Sqex::Sqpack::MemoryTextureEntryProvider>(pathSpec, StripSecondaryMipmaps(std::make_shared<Sqex::Sqpack::EntryRawStream>(std::move(src))), Z_NO_COMPRESSION);
	}
	throw std::runtime_error("Unknown entry type");
}

uint64_t StreamToFile(const Sqex::RandomAccessStream& stream, const Utils::Win32::Handle& file, std::vector<uint8_t>& buffer, uint64_t offset = 0, const std::string& progressPrefix = {}) {
	for (uint64_t pos = 0, pos_ = stream.StreamSize(); pos < pos_;) {
		std::cout << std::format("{} Save: {:>6.02f}%: {} / {}\n", progressPrefix, pos * 100. / pos_, pos, pos_);
		const auto read = static_cast<size_t>(stream.ReadStreamPartial(pos, &buffer[0], buffer.size()));
		file.Write(offset, std::span(buffer).subspan(0, read));
		pos += read;
		offset += read;
	}
	return offset;
}

void StreamToFile(const Sqex::RandomAccessStream& stream, const std::filesystem::path& file) {
	std::vector<uint8_t> buffer(1048576 * 16);
	StreamToFile(stream, Utils::Win32::Handle::FromCreateFile(file, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS), buffer, 0, 
		std::format("[{}]", file.filename().string()));
}

void UnpackTtmp(const std::filesystem::path srcDir, const std::filesystem::path& dstDir) {
	auto ttmp = Sqex::ThirdParty::TexTools::TTMPL();
	from_json(Utils::ParseJsonFromFile(srcDir / "TTMPL.mpl"), ttmp);

	const auto ttmpd = std::make_shared<Sqex::FileRandomAccessStream>(srcDir / "TTMPD.mpd");
	const auto ttmpd2 = Utils::Win32::Handle::FromCreateFile(dstDir / "TTMPD.mpd", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS);

	uint64_t offset = 0;
	std::vector<uint8_t> buffer(1048576 * 16);
	ttmp.ForEachEntry([&](Sqex::ThirdParty::TexTools::ModEntry& entry) {
		const auto provider = ToDecompressedEntryProvider(std::make_shared<Sqex::Sqpack::RandomAccessStreamAsEntryProviderView>(entry.FullPath, ttmpd, entry.ModOffset, entry.ModSize));
		entry.ModSize = provider->StreamSize();

		if ((offset + entry.ModSize) / 4096 != offset / 4096)
			offset = Sqex::Align<uint64_t>(offset, 4096).Alloc;

		entry.ModOffset = offset;

		std::cout << std::format("{}: {} ({} bytes)\n", entry.FullPath, entry.ModOffset, entry.ModSize);
		offset = StreamToFile(*provider, ttmpd2, buffer, offset);

		return Sqex::ThirdParty::TexTools::TTMPL::TraverseCallbackResult::Continue;
		});

	nlohmann::json out;
	to_json(out, ttmp);
	std::ofstream tout(dstDir / "TTMPL.mpl");
	tout << out;
}

std::string DumpSqtexInfo(std::span<uint8_t> buf) {
	using namespace Sqex::Sqpack::SqData;
	std::stringstream ss;

	const auto& entryHeader = *reinterpret_cast<FileEntryHeader*>(&buf[0]);
	ss << std::format("HeaderSize={} Type={} Decompressed={} AllocUnit={} OccupiedUnit={} Blocks={}\n",
		entryHeader.HeaderSize.Value(), static_cast<int>(entryHeader.Type.Value()), entryHeader.DecompressedSize.Value(),
		entryHeader.AllocatedSpaceUnitCount.Value(), entryHeader.OccupiedSpaceUnitCount.Value(), entryHeader.BlockCountOrVersion.Value());
	const auto locators = std::span(reinterpret_cast<TextureBlockHeaderLocator*>(&buf[sizeof entryHeader]), entryHeader.BlockCountOrVersion.Value());
	const auto subBlocks = std::span(reinterpret_cast<uint16_t*>(&buf[sizeof entryHeader + locators.size_bytes()]), locators.back().FirstSubBlockIndex.Value() + locators.back().SubBlockCount.Value());
	const auto& texHeader = *reinterpret_cast<Sqex::Texture::Header*>(&buf[entryHeader.HeaderSize]);
	ss << std::format("Unk1={} Header={} Type={} Width={} Height={} Layers={} Mipmaps={}\n",
		texHeader.Unknown1.Value(), texHeader.HeaderSize.Value(), static_cast<uint32_t>(texHeader.Type.Value()), texHeader.Width.Value(), texHeader.Height.Value(), texHeader.Layers.Value(), texHeader.MipmapCount.Value());
	const auto mipmapOffsets = std::span(reinterpret_cast<uint32_t*>(&buf[entryHeader.HeaderSize + sizeof texHeader]), texHeader.MipmapCount);

	ss << "Locators:\n";
	size_t lastOffset = 0;
	for (size_t i = 0; i < locators.size(); ++i) {
		const auto& l = locators[i];
		ss << std::format("\tOffset={} Size={} Decompressed={} Subblocks={}:{}\n", 
			l.FirstBlockOffset.Value(), l.TotalSize.Value(), l.DecompressedSize.Value(), l.FirstSubBlockIndex.Value(), l.FirstSubBlockIndex.Value() + l.SubBlockCount.Value());

		uint32_t baseRequestOffset = 0;
		if (i < mipmapOffsets.size()) {
			lastOffset = mipmapOffsets[i];
			ss << std::format("\tRequest={} (mipmapOffset)\n", lastOffset);
		} else if (i > 0) {
			ss << std::format("\tRequest={} (calc)\n", lastOffset);
		} else {
			lastOffset = 0;
			ss << std::format("\tRequest={} (first)\n", lastOffset);
		}
		auto pos = entryHeader.HeaderSize + l.FirstBlockOffset;
		for (auto j = l.FirstSubBlockIndex.Value(); j < l.FirstSubBlockIndex.Value() + l.SubBlockCount.Value(); ++j) {
			const auto& blockHeader = *reinterpret_cast<BlockHeader*>(&buf[pos]);
			ss << std::format("\t\tSub: Size={} HeaderSize={} Version={} Compressed={} Decompressed={}\n",
				subBlocks[j],
				blockHeader.HeaderSize.Value(), blockHeader.Version.Value(), blockHeader.CompressedSize.Value(), blockHeader.DecompressedSize.Value());
			pos += subBlocks[ j];
		}
		lastOffset += l.DecompressedSize;
	}

	return ss.str();
}

int main() {
	//const auto reader = Sqex::Sqpack::Reader(LR"(C:\Program Files (x86)\SquareEnix\FINAL FANTASY XIV - A Realm Reborn\game\sqpack\ffxiv\020000.win32.index)");

	//std::vector<uint8_t> buf, buf2, buf3;
	//std::map<Sqex::Sqpack::SqIndex::LEDataLocator, Sqex::Sqpack::SqIndex::LEDataLocator> locatorMap;
	//for (size_t i = 0; i < reader.EntryInfo.size(); ++i) {
	//	// if (i != 22883) continue;
	//	if (i != 22889) continue;
	//	//if (i != 164870) continue;
	//	//if (i != 174182) continue;
	//	const auto& [locator, entry] = reader.EntryInfo[i];
	//	if (!locator.Value)
	//		continue;

	//	std::cout << std::format("{:0>6}/{:0>6} Locator={:08x} FullPathHash={:08x} PathHash={:08x} NameHash={:08x}\n",
	//		i, reader.EntryInfo.size(), 
	//		locator.Value, entry.PathSpec.FullPathHash, entry.PathSpec.PathHash, entry.PathSpec.NameHash);

	//	buf.resize(entry.Allocation);
	//	reader.Data[locator.DatFileIndex].Stream->ReadStream(locator.DatFileOffset(), std::span(buf));
	//	auto& eh = *reinterpret_cast<Sqex::Sqpack::SqData::FileEntryHeader*>(&buf[0]);
	//	eh.AllocatedSpaceUnitCount = eh.OccupiedSpaceUnitCount;
	//	buf.resize(eh.GetTotalEntrySize());

	//	const auto provider = std::make_shared<Sqex::Sqpack::RandomAccessStreamAsEntryProviderView>(entry.PathSpec, std::make_shared<Sqex::MemoryRandomAccessStream>(std::span(buf)));
	//	if (provider->EntryType() != Sqex::Sqpack::SqData::FileEntryType::Texture)
	//		continue;

	//	const auto rawStream = std::make_shared<Sqex::Sqpack::EntryRawStream>(provider.get());
	//	buf2.resize(rawStream->StreamSize());
	//	rawStream->ReadStream(0, std::span(buf2));

	//	const auto decompressedEntryProvider = std::make_shared<Sqex::Sqpack::MemoryTextureEntryProvider>(entry.PathSpec, rawStream);
	//	buf3.resize(decompressedEntryProvider->StreamSize());
	//	decompressedEntryProvider->ReadStreamPartial(0, &buf3[0], buf3.size());

	//	if (memcmp(&buf[0], &buf3[0], buf3.size()) == 0)
	//		continue;

	//	DumpSqtexInfo(std::span(buf));
	//	DumpSqtexInfo(std::span(buf3));

	//	std::ofstream(std::format(L"Z:/sqpacktest/{}_{:0>6}_{:08x}_{:08x}_{:08x}_{:08x}.sqd", L"000000", i, locator.Value, entry.PathSpec.FullPathHash, entry.PathSpec.PathHash, entry.PathSpec.NameHash), std::ios::binary)
	//		.write(reinterpret_cast<const char*>(buf.data()), buf.size());
	//	std::ofstream(std::format(L"Z:/sqpacktest/{}_{:0>6}_{:08x}_{:08x}_{:08x}_{:08x}.tex", L"000000", i, locator.Value, entry.PathSpec.FullPathHash, entry.PathSpec.PathHash, entry.PathSpec.NameHash), std::ios::binary)
	//		.write(reinterpret_cast<const char*>(buf2.data()), buf2.size());
	//	std::ofstream(std::format(L"Z:/sqpacktest/{}_{:0>6}_{:08x}_{:08x}_{:08x}_{:08x}.rep", L"000000", i, locator.Value, entry.PathSpec.FullPathHash, entry.PathSpec.PathHash, entry.PathSpec.NameHash), std::ios::binary)
	//		.write(reinterpret_cast<const char*>(buf3.data()), buf3.size());
	//}

	Utils::Win32::TpEnvironment tpenv;
	for (const auto& index : std::filesystem::recursive_directory_iterator(std::filesystem::path(LR"(C:\Program Files (x86)\SquareEnix\FINAL FANTASY XIV - A Realm Reborn\game\sqpack\)"))) {
		if (index.path().extension() != ".index")
			continue;

		tpenv.SubmitWork([path = index.path()]() {
			const auto expac = path.parent_path().filename().string();
			const auto dir = std::filesystem::path(LR"(Z:\sqpacktest\game\sqpack)") / expac;
			std::filesystem::create_directories(dir);

			if (std::filesystem::exists(dir / path.filename()))
				return;

			Sqex::Sqpack::Creator creator(expac, std::filesystem::path(path).replace_extension("").replace_extension("").filename().string());
			creator.AddEntriesFromSqPack(path, true, true);

			const auto reader = Sqex::Sqpack::Reader(path);

			std::vector<uint8_t> buf, buf2, buf3;
			std::map<Sqex::Sqpack::SqIndex::LEDataLocator, Sqex::Sqpack::SqIndex::LEDataLocator> locatorMap;
			for (size_t i = 0; i < reader.EntryInfo.size(); ++i) {
				const auto& [locator, entry] = reader.EntryInfo[i];
				if (!locator.Value)
					continue;

				if (i % 64 == 0)
					std::cout << std::format("{:0>6}/{:0>6}_{}_{:08x}_{:08x}_{:08x}_{:08x}\n", i, reader.EntryInfo.size(), creator.DatName, locator.Value, entry.PathSpec.FullPathHash, entry.PathSpec.PathHash, entry.PathSpec.NameHash);

				buf.resize(static_cast<size_t>(entry.Allocation));
				reader.Data[locator.DatFileIndex].Stream->ReadStream(locator.DatFileOffset(), std::span(buf));
				auto& eh = *reinterpret_cast<Sqex::Sqpack::SqData::FileEntryHeader*>(&buf[0]);
				eh.AllocatedSpaceUnitCount = eh.OccupiedSpaceUnitCount;
				buf.resize(static_cast<size_t>(eh.GetTotalEntrySize()));

				auto ep = reader.GetEntryProvider(entry.PathSpec, locator, entry.Allocation);
				if (ep->EntryType() == Sqex::Sqpack::SqData::FileEntryType::EmptyOrObfuscated) {
					creator.AddEntry(ep);
					continue;
				}
				creator.AddEntry(ToDecompressedEntryProvider(ep));

				/*const auto provider = std::make_shared<Sqex::Sqpack::RandomAccessStreamAsEntryProviderView>(entry.PathSpec, std::make_shared<Sqex::MemoryRandomAccessStream>(std::span(buf)));
				auto rawStream = std::make_shared<Sqex::Sqpack::EntryRawStream>(provider.get());
				buf2.resize(rawStream->StreamSize());
				rawStream->ReadStream(0, std::span(buf2));

				const wchar_t* fileType;
				std::shared_ptr<Sqex::Sqpack::EntryProvider> decompressedEntryProvider;
				switch (rawStream->EntryType()) {
					case Sqex::Sqpack::SqData::FileEntryType::Binary:
						fileType = L"bin";
						decompressedEntryProvider = std::make_shared<Sqex::Sqpack::MemoryBinaryEntryProvider>(entry.PathSpec, std::move(rawStream));
						break;

					case Sqex::Sqpack::SqData::FileEntryType::Model:
						fileType = L"mdl";
						decompressedEntryProvider = std::make_shared<Sqex::Sqpack::MemoryModelEntryProvider>(entry.PathSpec, std::move(rawStream));
						break;

					case Sqex::Sqpack::SqData::FileEntryType::Texture:
						fileType = L"tex";
						decompressedEntryProvider = std::make_shared<Sqex::Sqpack::MemoryTextureEntryProvider>(entry.PathSpec, std::move(rawStream));
						break;

					default:
						continue;
				}

				buf3.resize(decompressedEntryProvider->StreamSize());
				decompressedEntryProvider->ReadStreamPartial(0, &buf3[0], buf3.size());

				auto ep = reader.GetEntryProvider(entry.PathSpec, locator, entry.Allocation);
				if (memcmp(&buf[0], &buf3[0], buf3.size()) == 0)
					creator.AddEntry(ToDecompressedEntryProvider(ep));
				else {
					creator.AddEntry(ep);
					if (provider->EntryType() == Sqex::Sqpack::SqData::FileEntryType::Texture) {
						auto s = DumpSqtexInfo(buf);
						std::ofstream(std::format(L"Z:/sqpacktest/{}_{:0>6}_{:08x}_{:08x}_{:08x}_{:08x}.sqd.txt", creator.DatName, i, locator.Value, entry.PathSpec.FullPathHash, entry.PathSpec.PathHash, entry.PathSpec.NameHash, fileType), std::ios::binary)
							.write(s.data(), s.size());
					}
					std::ofstream(std::format(L"Z:/sqpacktest/{}_{:0>6}_{:08x}_{:08x}_{:08x}_{:08x}.sqd", creator.DatName, i, locator.Value, entry.PathSpec.FullPathHash, entry.PathSpec.PathHash, entry.PathSpec.NameHash), std::ios::binary)
						.write(reinterpret_cast<const char*>(buf.data()), buf.size());

					std::ofstream(std::format(L"Z:/sqpacktest/{}_{:0>6}_{:08x}_{:08x}_{:08x}_{:08x}.{}", creator.DatName, i, locator.Value, entry.PathSpec.FullPathHash, entry.PathSpec.PathHash, entry.PathSpec.NameHash, fileType), std::ios::binary)
						.write(reinterpret_cast<const char*>(buf2.data()), buf2.size());

					if (provider->EntryType() == Sqex::Sqpack::SqData::FileEntryType::Texture) {
						auto s = DumpSqtexInfo(buf3);
						std::ofstream(std::format(L"Z:/sqpacktest/{}_{:0>6}_{:08x}_{:08x}_{:08x}_{:08x}.rep.txt", creator.DatName, i, locator.Value, entry.PathSpec.FullPathHash, entry.PathSpec.PathHash, entry.PathSpec.NameHash, fileType), std::ios::binary)
							.write(s.data(), s.size());
					}
					std::ofstream(std::format(L"Z:/sqpacktest/{}_{:0>6}_{:08x}_{:08x}_{:08x}_{:08x}.rep", creator.DatName, i, locator.Value, entry.PathSpec.FullPathHash, entry.PathSpec.PathHash, entry.PathSpec.NameHash), std::ios::binary)
						.write(reinterpret_cast<const char*>(buf3.data()), buf3.size());
				}*/
			}

			const auto views = creator.AsViews(false);
			if (views.Entries.empty()) {
				std::filesystem::copy(path, dir / path.filename());
				std::filesystem::copy(std::filesystem::path(path).replace_extension(".index2"), dir / path.filename().replace_extension(".index2"));
				std::filesystem::copy(std::filesystem::path(path).replace_extension(".dat0"), dir / path.filename().replace_extension(".dat0"));
				return;
			}

			StreamToFile(*views.Index1, (dir / path.filename()).string() + ".tmp");
			StreamToFile(*views.Index2, (dir / path.filename().replace_extension(".index2")).string() + ".tmp");
			for (size_t i = 0; i < views.Data.size(); ++i)
				StreamToFile(*views.Data[i], (dir / path.filename().replace_extension(std::format(".dat{}", i))).string() + ".tmp");

			std::filesystem::rename((dir / path.filename()).string() + ".tmp", dir / path.filename());
			std::filesystem::rename((dir / path.filename().replace_extension(".index2")).string() + ".tmp", dir / path.filename().replace_extension(".index2"));
			for (size_t i = 0; i < views.Data.size(); ++i)
				std::filesystem::rename((dir / path.filename().replace_extension(std::format(".dat{}", i))).string() + ".tmp",
					dir / path.filename().replace_extension(std::format(".dat{}", i)));
		});
	}
	tpenv.WaitOutstanding();
	return 0;
}