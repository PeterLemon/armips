#include "stdafx.h"
#include "Assembler.h"
#include "Core/Common.h"
#include "Commands/CAssemblerCommand.h"
#include "Commands/CAssemblerLabel.h"
#include "CMacro.h"
#include "Util/CommonClasses.h"
#include "Core/Directives.h"
#include "Archs/MIPS/Mips.h"

#define ASSEMBLER_MACRO_NESTING_LEVEL		128
#define ASSEMBLER_INCLUDE_NESTING_LEVEL		64

typedef struct {
	std::wstring buffer;
	std::wstring name;
	std::wstring params;
} tTextData;

inline bool CheckEndLine(std::wstring& string, int pos)
{
	if (pos >= string.size()) return true;

	if (string[pos] == 0) return true;
	if (string[pos] == '\n') return true;
	if (string[pos] == ';') return true;
	if (pos+1 < string.size() && string[pos+0] == '/' && string[pos+1] == '/') return true;
	return false;
}

bool GetLine(TextFile& Input, std::wstring& dest)
{
	std::wstring Buffer = Input.readLine();
	dest = L"";

	int InputPos = 0;

	while (InputPos < Buffer.size() && (Buffer[InputPos] == '\t' || Buffer[InputPos] == ' ')) InputPos++;
	while (InputPos < Buffer.size() && CheckEndLine(Buffer,InputPos) == false)
	{
		switch (Buffer[InputPos])
		{
		case ' ':
		case '\t':
			while (InputPos < Buffer.size() && (Buffer[InputPos] == '\t' || Buffer[InputPos] == ' ')) InputPos++;
			if (CheckEndLine(Buffer,InputPos) == true)
			{
				return true;
			}
			dest += ' ';
			break;
		case '"':	// string
			dest += towlower(Buffer[InputPos++]);
			while (Buffer[InputPos] != '"')
			{
				if (Buffer[InputPos] == '\\' && Buffer[InputPos+1] == '"')
				{
					dest += '\\';
					dest += '"';
					InputPos += 2;
					continue;
				}
				if (Buffer[InputPos] == '\\' && Buffer[InputPos+1] == '\\')
				{
					dest += '\\';
					dest += '\\';
					InputPos += 2;
					continue;
				}
				if (Buffer[InputPos] == '\n' || Buffer[InputPos] == 0)
				{
					PrintError(ERROR_ERROR,"Unexpected end of line in string constant");
					return false;
				}
				dest += Buffer[InputPos++];
			}
			dest += towlower(Buffer[InputPos++]);
			break;
		case '\'':
			if (Buffer[InputPos+2] == '\'')
			{
				dest += Buffer[InputPos++];
				dest += Buffer[InputPos++];
				dest += Buffer[InputPos++];
			} else {
				PrintError(ERROR_ERROR,"Invalid character constant");
				return false;
			}
			break;
		default:
			dest += towlower(Buffer[InputPos++]);
			break;
		}
	}

	return true;
}

bool CheckEquLabel(std::wstring& str)
{
	size_t s = str.find(L" equ ");
	if (s == std::string::npos) s = str.find(L":equ ");
	if (s != std::string::npos)
	{
		std::wstring name = str.substr(0,s);
		if (name.back() == ':') name.pop_back();

		if (Global.symbolTable.isValidSymbolName(name) == false)
		{
			PrintError(ERROR_ERROR,"Invalid equation name %s",str);
			return true;
		}

		if (Global.symbolTable.symbolExists(name,Global.FileInfo.FileNum,Global.Section))
		{
			PrintError(ERROR_ERROR,"Equation name %s already defined",str);
			return true;
		}
		
		std::wstring replacement = str.substr(s+5);
		Global.symbolTable.addEquation(name,Global.FileInfo.FileNum,Global.Section,replacement);
		return true;
	}

	return false;
}

std::wstring checkLabel(std::wstring& str, bool AllLocal)
{
	int pos = 0;

	while (pos < str.size() && str[pos] != ' ' && str[pos] != 0)
	{
		if (str[pos] == ':')
		{
			std::wstring name = str.substr(0,pos);
			if (AllLocal == true && Global.symbolTable.isGlobalSymbol(name))
				name = L"@@" + name;
			
			addAssemblerLabel(name);
			return str.substr(pos+1);
		}
		pos++;
	}

	return str;
}

void splitLine(std::wstring& line, std::wstring& name, std::wstring& arguments)
{
	int linePos = 0;
	name = L"";
	arguments = L"";

	while (linePos < line.size() && (line[linePos] == ' ' || line[linePos] == '\t')) linePos++;
	while (linePos < line.size() && line[linePos] != ' ')
	{
		if (line[linePos]  == 0)
		{
			return;
		}
		name.push_back(line[linePos++]);
	}
	
	while (linePos < line.size() && (line[linePos] == ' ' || line[linePos] == '\t')) linePos++;

	while (linePos < line.size() && line[linePos] != 0)
	{
		arguments.push_back(line[linePos++]);
	}
}

void AddFileName(char* FileName)
{
	Global.FileInfo.FileNum = Global.FileInfo.FileList.GetCount();
	Global.FileInfo.FileList.AddEntry(FileName);
	Global.FileInfo.LineNumber = 0;
}

void InsertMacro(CMacro* Macro, std::wstring& Args)
{
	tTextData Text;
	ArgumentList Arguments;

	splitArguments(Arguments,Args);

	if (Arguments.size() != Macro->getArgumentCount())
	{
		PrintError(ERROR_ERROR,"%s macro arguments (%d vs %d)",
			Arguments.size() > Macro->getArgumentCount() ? "Too many" : "Not enough",
			Arguments.size(),Macro->getArgumentCount());
		return;
	}

	Global.MacroNestingLevel++;
	if (Global.MacroNestingLevel == ASSEMBLER_MACRO_NESTING_LEVEL)
	{
		PrintError(ERROR_ERROR,"Maximum macro nesting level reached");
		return;
	}

	int MacroCounter = Macro->getIncreaseCounter();

	for (int i = 0; i < Macro->getLineCount(); i++)
	{
		Text.buffer = Macro->getLine(i,Arguments,MacroCounter);
		Text.buffer = Global.symbolTable.insertEquations(Text.buffer,Global.FileInfo.FileNum,Global.Section);

		if (CheckEquLabel(Text.buffer) == false)
		{
			Text.buffer = checkLabel(Text.buffer,false);
			splitLine(Text.buffer,Text.name,Text.params);
			if (Text.name.size() == 0) continue;

			bool macro = false;
			for (size_t i = 0; i < Global.Macros.size(); i++)
			{
				if (Text.name.compare(Global.Macros[i]->getName()) == 0)
				{
					InsertMacro(Global.Macros[i],Text.params);
					macro = true;
				}
			}
			if (macro == true) continue;

			if (Arch->AssembleDirective(Text.name,Text.params) == false)
			{
				Arch->AssembleOpcode(Text.name,Text.params);
			}
		}
	}
	Global.MacroNestingLevel--;
}

bool ParseMacro(TextFile& Input, std::wstring& opcodeName, std::wstring& Args)
{
	if (opcodeName.compare(L".macro") == 0)
	{
		parseMacroDefinition(Input,Args);
		return true;
	}

	for (size_t i = 0; i < Global.Macros.size(); i++)
	{
		if (opcodeName.compare(Global.Macros[i]->getName()) == 0)
		{
			Global.MacroNestingLevel = 0;
			InsertMacro(Global.Macros[i],Args);
			return true;
		}
	}
	return false;
}

void parseMacroDefinition(TextFile& Input, std::wstring& Args)
{
	tTextData Text;
	ArgumentList Arguments;

	splitArguments(Arguments,Args);

	CMacro* Macro = new CMacro();
	Macro->loadArguments(Arguments);

	while (true)
	{
		if (Input.atEnd())
		{
			PrintError(ERROR_ERROR,"Unexpected end of line in macro definition");
			return;
		}
		Global.FileInfo.LineNumber++;
		if (GetLine(Input,Text.buffer) == false) continue;
		if (Text.buffer.empty()) continue;
		splitLine(Text.buffer,Text.name,Text.params);
		if (Text.name.compare(L".endmacro") == 0) break;
		Macro->addLine(Text.buffer);
	}

	for (size_t i = 0; i < Global.Macros.size(); i++)
	{
		if (Macro->getName().compare(Global.Macros[i]->getName()) == 0)
		{
			PrintError(ERROR_ERROR,"Macro \"%ls\" already defined",Macro->getName().c_str());
			delete Macro;
			return;
		}
	}

	Global.Macros.push_back(Macro);
}

void LoadAssemblyFile(std::wstring& fileName)
{
	tTextData Text;
	CStringList Arguments;
	int num = 0;

	AddFileName((char*)convertWStringToUtf8(fileName).c_str());
	Global.IncludeNestingLevel++;

	if (Global.IncludeNestingLevel == ASSEMBLER_INCLUDE_NESTING_LEVEL)
	{
		PrintError(ERROR_ERROR,"Maximum include nesting level reached");
		return;
	}

	TextFile input;
	if (input.open(fileName,TextFile::Read) == false)
	{
		PrintError(ERROR_ERROR,"Could not open file");
		return;
	}

	while (!input.atEnd())
	{
		Global.FileInfo.LineNumber++;
		Global.FileInfo.TotalLineCount++;

		if (GetLine(input,Text.buffer) == false) continue;
		if (Text.buffer.size() == 0) continue;
		
		Text.buffer = Global.symbolTable.insertEquations(Text.buffer,Global.FileInfo.FileNum,Global.Section);

		if (CheckEquLabel(Text.buffer) == false)
		{
			Text.buffer = checkLabel(Text.buffer,false);
			splitLine(Text.buffer,Text.name,Text.params);
			if (Text.name.empty()) continue;

			if (ParseMacro(input,Text.name,Text.params) == true) continue;
			if (Arch->AssembleDirective(Text.name,Text.params) == false)
			{
				Arch->AssembleOpcode(Text.name,Text.params);
			}
		}
	}

	Global.IncludeNestingLevel--;
	input.close();
}

bool EncodeAssembly()
{
	bool Revalidate;
	Arch->Pass2();
	int validationPasses = 0;
	do	// loop until everything is constant
	{
		Global.validationPasses = validationPasses;
		Global.ErrorQueue.Clear();
		Revalidate = false;

		if (validationPasses >= 100)
		{
			QueueError(ERROR_ERROR,"Stuck in infinite validation loop");
			break;
		}

		Global.RamPos = 0;
		Arch->Revalidate();

#ifdef _DEBUG
		printf("Validate %d...\n",validationPasses);
#endif

		for (size_t i = 0; i < Global.Commands.size(); i++)
		{
			if (Global.Commands[i]->IsConditional() == false)
			{
				if (Global.conditionData.conditionTrue() == false)
					continue;
			}

			Global.Commands[i]->SetFileInfo();

			for (int l = 0; l < Global.AreaData.EntryCount; l++)
			{
				if (Global.AreaData.Entries[l].MaxRamPos < Global.RamPos)
				{
					QueueError(ERROR_ERROR,"Area at %s(%d) overflown",
						Global.FileInfo.FileList.GetEntry(Global.AreaData.Entries[l].FileNum),
						Global.AreaData.Entries[l].LineNumber);
				}
			}

			if (Global.Commands[i]->Validate() == true)
			{
				Revalidate = true;
			}
		}
		if (Global.conditionData.activeConditions() != 0)
			QueueError(ERROR_ERROR,"One or more if statements not terminated");
		validationPasses++;
	} while (Revalidate == true);

	Global.ErrorQueue.Output();

	if (Global.Error == true)
	{
		return false;
	}

#ifdef _DEBUG
	printf("Encode...\n");
#endif

	// and finally encode
	Global.tempData.start();
	Global.symData.start();
	for (size_t i = 0; i < Global.Commands.size(); i++)
	{
		if (Global.Commands[i]->IsConditional() == false && Global.conditionData.conditionTrue() == false)
		{
			delete Global.Commands[i];
			continue;
		}

		Global.Commands[i]->SetFileInfo();
		Global.Commands[i]->Encode();
		Global.Commands[i]->writeTempData(Global.tempData);
		Global.Commands[i]->writeSymData(Global.symData);
		delete Global.Commands[i];
	}

	Global.tempData.end();
	Global.symData.end();

	if (Global.Output.isOpen() == true)
	{
		PrintError(ERROR_WARNING,"File not closed");
		Global.Output.close();
	}

	return true;
}
